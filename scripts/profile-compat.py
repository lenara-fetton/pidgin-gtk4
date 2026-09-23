#!/usr/bin/env python3
"""Order-insensitive comparison of two libpurple profile directories.

Helper for scripts/check-profile-compat.sh (the cutover-gate round-trip in
doc/PIDGIN-UPGRADE.md). Subcommands:

  flatten FILE.xml         print FILE as sorted "path attrs = text" lines
  counts PROFILE           print account/buddy/contact/group/chat/pref counts
  compare BASE NEW [--allow KIND:REGEX]... [--verbose]
                           compare two profile directories; print every
                           change that the rules below do not allow and exit
                           1 if there is one
  simulate PROFILE [--unexpected]
                           make the edits a UI session would make (used by
                           the shell script's --dry-run/--self-test); with
                           --unexpected, also make edits compare must reject

Why flatten: libpurple keeps account settings, blist node settings and prefs
in hash tables, so their order in the XML changes between saves. Every
element is identified by a path built from stable keys (the name attribute,
an account's protocol and <name>, a buddy's account and <name>, chat
components, a transient status' creation time, ...), not by its position.

A change is one of:
  added    the path exists only in NEW
  removed  the path exists only in BASE
  changed  the path exists in both, with different attributes or text
Rules are "KIND:REGEX" with KIND one of added, removed, changed or any. The
regex is matched (re.search) against "FILE:LINE", where LINE is the flattened
line from BASE for removed/changed and from NEW for added.
"""

import argparse
import filecmp
import os
import re
import sys
import xml.etree.ElementTree as ET

XML_FILES = ('accounts.xml', 'blist.xml', 'prefs.xml', 'status.xml', 'pounces.xml')

# Account/blist settings whose values legitimately change during a session.
VOLATILE_SETTINGS = (
	'signedon', 'signedoff', 'last_seen', 'lastseen', 'icon_checksum',
	'buddy_icon', 'buddy_icon_timestamp', 'avatar_hash', 'last_message_id',
	'last_message_id_high', 'last_message_id_low', 'last_message_timestamp',
	'gtk-mute-sound',
)

DEFAULT_RULES = [
	# status.xml: transient ("Auto-Cached") statuses are created on every
	# status change and the oldest are pruned. Running with --nologin makes
	# the offline status current, which creates one.
	('any', r'^status\.xml:/statuses/status\[Auto-Cached'),
	# prefs.xml: which saved status is default/idle follows the above.
	('changed', r'^prefs\.xml:/pref/pref\[purple\]/pref\[savedstatus\]/'),
	# prefs.xml: window geometry.
	('changed', r'^prefs\.xml:/pref/pref\[pidgin\]/.*/pref\[(x|y|width|height|pane_pos)\]( |$)'),
	# prefs.xml: pidgin4's own subtree (contract rule 2) may do anything.
	('any', r'^prefs\.xml:/pref/pref\[pidgin4\]'),
	# prefs.xml: libpurple plugins in the private prefix (e.g. omemo) register
	# their own /plugins/core/<id> defaults on first load. Additive keys are
	# allowed by contract rule 2; removals and changes are still caught.
	('added', r'^prefs\.xml:/pref/pref\[plugins\]/pref\[core\]/'),
	# accounts.xml: per-account presence (active flags, status messages).
	('any', r'^accounts\.xml:/account/account\[[^]]*\]/statuses/'),
	('any', r'^accounts\.xml:/account/account\[[^]]*\]/current_error'),
	# accounts.xml: Pidgin 2 drops <setting>s that have no type attribute.
	('removed', r'^accounts\.xml:.*/setting\[[^]]*\]$'),
	# New keys are additive (contract rule 2).
	('added', r'^(accounts|blist)\.xml:.*/setting\[[^]]*\]( |$)'),
	# Session state kept in settings.
	('changed', r'^(accounts|blist)\.xml:.*/setting\[(%s)\]( |$)' % '|'.join(VOLATILE_SETTINGS)),
	('removed', r'^blist\.xml:.*/setting\[(%s)\]( |$)' % '|'.join(VOLATILE_SETTINGS)),
	# blist.xml: a live session receives roster pushes and adds buddies
	# (libpurple does this identically in both UIs). Removing a buddy,
	# contact or group is still an error, and the counts must not shrink.
	('added', r'^blist\.xml:/purple/blist/group(\[[^]]*\])?/contact\['),
	('added', r'^blist\.xml:/purple/blist/group(\[[^]]*\])?/chat\['),
]

# Files outside the five XML files. Paths are relative to the profile root.
FILE_RULES = [
	# pidgin4's directory (contract rule 5).
	('any', r'^pidgin4(/|$)'),
	# New log files and appended logs; deleting logs is never allowed.
	('added', r'^logs/'),
	('changed', r'^logs/'),
	# Caches and plugin data rewritten while running.
	('any', r'^xmpp-caps\.xml$'),
	('changed', r'^cap\.db$'),
	('added', r'^icons/'),
	('added', r'^certificates/'),
]


def child_text(elem, tag):
	c = elem.find(tag)
	return (c.text or '').strip() if c is not None else ''


def element_key(elem):
	tag = elem.tag
	a = elem.attrib
	if tag == 'account' and elem.find('protocol') is not None:
		return 'account[%s:%s]' % (child_text(elem, 'protocol'), child_text(elem, 'name'))
	if tag == 'buddy':
		return 'buddy[%s:%s:%s]' % (a.get('proto', ''), a.get('account', ''), child_text(elem, 'name'))
	if tag == 'chat':
		comps = sorted('%s=%s' % (c.get('name', ''), (c.text or '').strip())
		               for c in elem.findall('component'))
		return 'chat[%s:%s:%s]' % (a.get('proto', ''), a.get('account', ''), ','.join(comps))
	if tag == 'contact':
		# Contacts have no id; name them after their first buddy (in sorted
		# order) so that adding or removing another member does not rename
		# the contact and every buddy below it.
		names = sorted('%s:%s' % (b.get('account', ''), child_text(b, 'name'))
		               for b in elem.findall('buddy'))
		return 'contact[%s]' % (names[0] if names else '')
	if tag == 'status' and a.get('transient') == 'true':
		return 'status[Auto-Cached:%s]' % a.get('created', '')
	if tag == 'item':
		return 'item[%s]' % a.get('value', (elem.text or '').strip())
	keys = [a[k] for k in ('name', 'ui', 'account', 'proto', 'protocol', 'id')
	        if k in a]
	if tag not in ('setting', 'pref') and 'type' in a:
		keys.append(a['type'])
	return '%s[%s]' % (tag, ':'.join(keys)) if keys else tag


KEY_ATTRS = {'name', 'ui', 'account', 'proto', 'protocol', 'id'}


def flatten(path):
	"""Return {path: line} for every element of the XML file."""
	root = ET.parse(path).getroot()
	out = {}

	def walk(elem, epath):
		counts = {}
		for child in elem:
			if not isinstance(child.tag, str):
				continue
			key = element_key(child)
			n = counts.get(key, 0)
			counts[key] = n + 1
			if n:
				key = '%s#%d' % (key, n)
			cpath = '%s/%s' % (epath, key)
			attrs = ' '.join('%s=%s' % (k, v) for k, v in sorted(child.attrib.items())
			                 if k not in KEY_ATTRS)
			text = (child.text or '').strip()
			line = cpath
			if attrs:
				line += ' ' + attrs
			if text:
				line += ' = ' + text.replace('\n', '\\n')
			out[cpath] = line
			walk(child, cpath)

	walk(root, '/' + root.tag)
	return out


def counts(profile):
	res = {}

	def load(name):
		p = os.path.join(profile, name)
		return ET.parse(p).getroot() if os.path.exists(p) else None

	acc = load('accounts.xml')
	res['accounts'] = len(acc.findall('account')) if acc is not None else 0
	bl = load('blist.xml')
	if bl is not None:
		res['groups'] = len(bl.findall('./blist/group'))
		res['contacts'] = len(bl.findall('./blist/group/contact'))
		res['buddies'] = len(bl.findall('./blist/group/contact/buddy'))
		res['chats'] = len(bl.findall('./blist/group/chat'))
		res['privacy'] = len(bl.findall('./privacy/account'))
	pr = load('prefs.xml')
	res['prefs'] = len(pr.findall('.//pref')) if pr is not None else 0
	st = load('status.xml')
	res['saved-statuses'] = len([s for s in st.findall('status')
	                             if s.get('transient') != 'true']) if st is not None else 0
	po = load('pounces.xml')
	res['pounces'] = len(po.findall('pounce')) if po is not None else 0
	return res


def match(rules, kind, subject):
	for rkind, rx in rules:
		if rkind in (kind, 'any') and re.search(rx, subject):
			return True
	return False


def compare_xml(name, base, new, rules, report):
	b = flatten(base)
	n = flatten(new)
	bad = 0
	for path in sorted(set(b) | set(n)):
		if path in b and path in n:
			if b[path] == n[path]:
				continue
			kind, subject = 'changed', b[path]
			detail = '%s\n      -> %s' % (b[path], n[path])
		elif path in b:
			kind, subject, detail = 'removed', b[path], b[path]
		else:
			kind, subject, detail = 'added', n[path], n[path]
		ok = match(rules, kind, '%s:%s' % (name, subject))
		report(ok, '%-7s %s: %s' % (kind, name, detail))
		bad += not ok
	return bad


def walk_files(root):
	res = {}
	for dirpath, dirnames, filenames in os.walk(root):
		for f in filenames:
			full = os.path.join(dirpath, f)
			res[os.path.relpath(full, root)] = full
	return res


def compare(args):
	rules = list(DEFAULT_RULES) + [tuple(r.split(':', 1)) for r in args.allow]
	frules = list(FILE_RULES) + [tuple(r.split(':', 1)) for r in args.allow_file]
	for r in rules + frules:
		if len(r) != 2 or r[0] not in ('added', 'removed', 'changed', 'any'):
			sys.exit('bad rule %r (want KIND:REGEX)' % (':'.join(r),))

	allowed = []
	unexpected = []

	def report(ok, msg):
		(allowed if ok else unexpected).append(msg)

	bad = 0
	# 1. The profile XML files, semantically.
	for name in XML_FILES:
		bp = os.path.join(args.base, name)
		np = os.path.join(args.new, name)
		if os.path.exists(bp) and not os.path.exists(np):
			report(False, 'removed %s' % name)
			bad += 1
		elif os.path.exists(bp):
			bad += compare_xml(name, bp, np, rules, report)
		elif os.path.exists(np):
			report(True, 'added   %s' % name)

	# 2. Every other file, by content.
	bf = walk_files(args.base)
	nf = walk_files(args.new)
	for rel in sorted(set(bf) | set(nf)):
		if rel in XML_FILES:
			continue
		if rel in bf and rel in nf:
			if filecmp.cmp(bf[rel], nf[rel], shallow=False):
				continue
			kind = 'changed'
		elif rel in bf:
			kind = 'removed'
		else:
			kind = 'added'
		ok = match(frules, kind, rel)
		report(ok, '%-7s file %s' % (kind, rel))
		bad += not ok

	# 3. Object counts must not change.
	cb = counts(args.base)
	cn = counts(args.new)
	for k in sorted(set(cb) | set(cn)):
		if k == 'prefs':
			continue  # pidgin4 prefs are additive; removals are caught above
		if cb.get(k) != cn.get(k):
			report(False, 'count   %s: %s -> %s' % (k, cb.get(k), cn.get(k)))
			bad += 1

	if args.verbose and allowed:
		print('-- allowed changes (%d):' % len(allowed))
		for m in allowed:
			print('   ' + m)
	if unexpected:
		print('-- UNEXPECTED changes (%d):' % len(unexpected))
		for m in unexpected:
			print('   ' + m)
	print('-- %d allowed, %d unexpected change(s)' % (len(allowed), len(unexpected)))
	return 1 if bad else 0


def simulate(args):
	"""Edit a scratch profile the way a UI session would (for --dry-run).

	Expected edits: a new saved-status default, a /pidgin4 pref subtree, a new
	account setting, a pidgin4/ directory. With --unexpected, also drop a
	buddy and add an entry to /pidgin/plugins/loaded, both of which the
	compare step must reject.
	"""
	import time
	prof = args.profile
	now = str(int(time.time()))

	p = os.path.join(prof, 'prefs.xml')
	tree = ET.parse(p)
	root = tree.getroot()
	for e in root.iter('pref'):
		if e.get('name') == 'savedstatus':
			for c in e.findall('pref'):
				if c.get('name') == 'default':
					c.set('value', now)
	p4 = ET.SubElement(root, 'pref', name='pidgin4')
	ET.SubElement(p4, 'pref', name='simulated', type='bool', value='1')
	if args.unexpected:
		for e in root.iter('pref'):
			if e.get('name') == 'loaded' and e.get('type') == 'pathlist':
				ET.SubElement(e, 'item', value='/nonexistent/pidgin4-plugin.so')
				break
	tree.write(p, encoding='UTF-8', xml_declaration=True)

	p = os.path.join(prof, 'accounts.xml')
	tree = ET.parse(p)
	acc = tree.getroot().find('account')
	if acc is not None:
		settings = acc.find('settings')
		if settings is None:
			settings = ET.SubElement(acc, 'settings')
		s = ET.SubElement(settings, 'setting', name='pidgin4-simulated', type='bool')
		s.text = '1'
		tree.write(p, encoding='UTF-8', xml_declaration=True)

	if args.unexpected:
		p = os.path.join(prof, 'blist.xml')
		tree = ET.parse(p)
		for contact in tree.getroot().iter('contact'):
			buddies = contact.findall('buddy')
			if len(buddies) > 1:
				contact.remove(buddies[-1])
				break
		tree.write(p, encoding='UTF-8', xml_declaration=True)

	os.makedirs(os.path.join(prof, 'pidgin4'), exist_ok=True)
	with open(os.path.join(prof, 'pidgin4', 'messages.db'), 'w') as f:
		f.write('simulated\n')
	return 0


def main():
	ap = argparse.ArgumentParser(description=__doc__,
	                             formatter_class=argparse.RawDescriptionHelpFormatter)
	sub = ap.add_subparsers(dest='cmd', required=True)
	p = sub.add_parser('flatten')
	p.add_argument('file')
	p = sub.add_parser('counts')
	p.add_argument('profile')
	p = sub.add_parser('compare')
	p.add_argument('base')
	p.add_argument('new')
	p.add_argument('--allow', action='append', default=[], metavar='KIND:REGEX',
	               help='extra rule for the XML files')
	p.add_argument('--allow-file', action='append', default=[], metavar='KIND:REGEX',
	               help='extra rule for other files (regex on the relative path)')
	p.add_argument('--verbose', '-v', action='store_true')
	p = sub.add_parser('simulate')
	p.add_argument('profile')
	p.add_argument('--unexpected', action='store_true')
	args = ap.parse_args()

	if args.cmd == 'simulate':
		return simulate(args)

	if args.cmd == 'flatten':
		for _, line in sorted(flatten(args.file).items()):
			print(line)
		return 0
	if args.cmd == 'counts':
		for k, v in counts(args.profile).items():
			print('%s=%s' % (k, v))
		return 0
	return compare(args)


if __name__ == '__main__':
	sys.exit(main())
