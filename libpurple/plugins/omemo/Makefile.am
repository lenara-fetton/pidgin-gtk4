EXTRA_DIST =

plugindir = $(libdir)/purple-$(PURPLE_MAJOR_VERSION)

omemo_la_LDFLAGS = -module -avoid-version

if PLUGINS

plugin_LTLIBRARIES = omemo.la

omemo_la_SOURCES = \
	omemo.c \
	omemo.h \
	omemo-crypto.c \
	omemo-store.c \
	omemo-xmpp.c

# Only the plugin links these; libpurple itself gains no dependency.
omemo_la_LIBADD = $(GLIB_LIBS) $(OMEMO_LIBS)

endif # PLUGINS

AM_CPPFLAGS = \
	-DDATADIR=\"$(datadir)\" \
	-I$(top_srcdir)/libpurple \
	-I$(top_builddir)/libpurple \
	$(DEBUG_CFLAGS) \
	$(GLIB_CFLAGS) \
	$(OMEMO_CFLAGS) \
	$(PLUGIN_CFLAGS)
