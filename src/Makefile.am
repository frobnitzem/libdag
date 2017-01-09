#AM_CFLAGS = -Wno-comment -Wno-pointer-sign -Wno-logical-op-parentheses # clang
ACLOCAL_AMFLAGS = -I m4

EXTLIBS =
AM_CPPFLAGS = 
AM_CFLAGS = -Wno-comment -Wno-logical-op-parentheses # -Wno-deprecated-declarations

include_HEADERS = dag.h dag_thread.h

lib_LTLIBRARIES = libdag.la
libdag_la_SOURCES = dag.c

libdag_la_LIBADD = $(EXTLIBS)
