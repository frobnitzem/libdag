AC_INIT([dag], [1.0], [predictivestatmech@gmail.com])
AC_PREREQ([2.65])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([-Wall -Werror])

AC_CONFIG_MACRO_DIRS([m4])

# sanity check.
AC_CONFIG_SRCDIR([src/dag.c])

# Check for programs.
AC_PROG_CC
AM_PROG_AR
AC_PROG_LIBTOOL

dnl Check for libraries.
dnl ACX_PTHREAD

AX_PTHREAD([AC_DEFINE([HAVE_PTHREAD], [1], [pthreads])],
           [AC_MSG_ERROR([libdag requires pthreads])
])

LIBS="$PTHREAD_LIBS $LIBS"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS -std=c11"
CC="$PTHREAD_CC"

#LIBS="$PTHREAD_LIBS $LIBS"
#CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
#CC="$PTHREAD_CC"

# Check for header files.
AC_CHECK_HEADERS([stdint.h stdlib.h string.h unistd.h])

# Output files.
AC_CONFIG_HEADERS([config.h])

AC_CONFIG_FILES([dag.pc Makefile src/Makefile test/Makefile])
AC_REQUIRE_AUX_FILE([tap-driver.sh])
AC_OUTPUT
