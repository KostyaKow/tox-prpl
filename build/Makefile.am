EXTRA_DIST = \
	$(top_srcdir)/README

TOXSOURCES = $(top_srcdir)/src/toxprpl.c

libtox_la_LDFLAGS = -module -avoid-version

# todo: check and pass value from configure
PURPLE_MAJOR_VERSION = "2"

pkgdir = $(libdir)/purple-$(PURPLE_MAJOR_VERSION)

pkg_LTLIBRARIES    = libtox.la
libtox_la_SOURCES = $(TOXSOURCES)
libtox_la_CFLAGS = 	-I$(top_srcdir) \
					-I$(top_srcdir)/src \
					$(GLIB_CFLAGS) \
					$(PURPLE_CFLAGS) \
					$(LIBTOXCORE_CFLAGS)

libtox_la_LIBADD  = $(LIBTOXCORE_LDFLAGS) \
					$(GLIB_LIBS) \
					$(PURPLE_LIBS) \
					$(LIBTOXCORE_LIBS)

