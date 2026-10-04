AUTOMAKE_OPTIONS = foreign

noinst_LIBRARIES = libsosapi.a

libsosapi_a_SOURCES = sysclipboard.cc sysfile.cc \
syscdrom.cc sysethtun.cc sysinit.cc systhread.cc systimer.cc types.h

AM_CPPFLAGS = -I$(top_builddir)/src -I$(top_srcdir)/src

CLEANFILES = sysethtun.cc

sysethtun.cc: $(srcdir)/../posix/sysethtun.cc
	ln -sf "$(abs_srcdir)/../posix/sysethtun.cc" "$@"
