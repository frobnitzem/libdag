ACLOCAL_AMFLAGS = -I m4

LDADD = ../src/libdag.la
AM_CPPFLAGS = -I$(top_srcdir)/src

TEST_LOG_DRIVER = env AM_TAP_AWK='$(AWK)' $(SHELL) \
		  $(top_srcdir)/build-aux/tap-driver.sh

TESTS = dtest.test

check_PROGRAMS = dtest.test
dtest_test_SOURCES = dtest.c

