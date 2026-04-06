AC_PREREQ([2.69])
AC_INIT([libwyoming], [0.1.0], [https://github.com/briankwest/libwyoming/issues])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall dist-xz])

AM_PROG_AR
LT_INIT

AC_PROG_CC
AC_PROG_INSTALL

# Optional: Piper TTS engine
AC_ARG_ENABLE([piper],
  AS_HELP_STRING([--enable-piper], [Build Piper TTS engine (requires libpiper)]),
  [enable_piper=$enableval], [enable_piper=auto])

have_piper=no
if test "x$enable_piper" != "xno"; then
  AC_CHECK_LIB([piper], [piper_create], [have_piper=yes], [have_piper=no])
  AC_CHECK_LIB([onnxruntime], [OrtGetApiBase], [], [have_piper=no])
  if test "x$enable_piper" = "xyes" -a "x$have_piper" = "xno"; then
    AC_MSG_ERROR([Piper requested but libpiper/libonnxruntime not found])
  fi
fi
AM_CONDITIONAL([HAVE_PIPER], [test "x$have_piper" = "xyes"])
if test "x$have_piper" = "xyes"; then
  AC_DEFINE([WYOMING_HAVE_PIPER], [1], [Piper TTS engine available])
fi

AC_CHECK_FUNC([epoll_create1], [AC_DEFINE([HAVE_EPOLL], [1], [Have epoll])])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
    Makefile
    src/Makefile
    include/Makefile
    tools/Makefile
    tests/Makefile
    libwyoming.pc
])
AC_OUTPUT
