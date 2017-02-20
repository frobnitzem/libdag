ACLOCAL_AMFLAGS = -I m4

LDADD = ../src/libdag.la -lm -lpthread
AM_CPPFLAGS = -I$(top_srcdir)/src

TEST_LOG_DRIVER = env AM_TAP_AWK='$(AWK)' $(SHELL) \
		  $(top_srcdir)/build-aux/tap-driver.sh

TESTS = dtest.test rtest.test

check_PROGRAMS = dtest.test rtest.test
dtest_test_SOURCES = dtest.c
rtest_test_SOURCES = rtest.c

