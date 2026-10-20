#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([gnome-voice-server],[1.0.0],[ole.aamot@aamotinnovation.com],
        [gnome-voice-server],[https://www.gnomevoice.org/])

AC_CONFIG_SRCDIR([src/gvs-main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([1.11 foreign -Wall -Wno-portability no-dist-gzip dist-xz tar-ustar subdir-objects color-tests parallel-tests])
AM_SILENT_RULES([yes])

AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB
AC_USE_SYSTEM_EXTENSIONS
PKG_PROG_PKG_CONFIG

GLIB_REQUIRED=2.66.0
PKG_CHECK_MODULES([GVS],
                  [glib-2.0 >= $GLIB_REQUIRED
                   gobject-2.0 >= $GLIB_REQUIRED
                   gio-2.0 >= $GLIB_REQUIRED
                   gio-unix-2.0 >= $GLIB_REQUIRED])

AC_ARG_ENABLE([warnings],
              [AS_HELP_STRING([--disable-warnings], [do not add extra compiler warnings])],
              [], [enable_warnings=yes])
WARN_CFLAGS=""
AS_IF([test "x$enable_warnings" = "xyes" && test "x$GCC" = "xyes"],
      [WARN_CFLAGS="-Wall -Wextra -Wno-unused-parameter -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes"])
AC_SUBST([WARN_CFLAGS])

# systemd unit directory (optional)
AC_ARG_WITH([systemdsystemunitdir],
            [AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [directory for systemd service files (or "no")])],
            [], [with_systemdsystemunitdir='${prefix}/lib/systemd/system'])
AS_IF([test "x$with_systemdsystemunitdir" != "xno"],
      [AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])])
AM_CONDITIONAL([HAVE_SYSTEMD], [test "x$with_systemdsystemunitdir" != "xno"])

AC_ARG_WITH([storagedir],
            [AS_HELP_STRING([--with-storagedir=DIR], [default voicegram storage directory @<:@LOCALSTATEDIR/lib/gnome-voice-server@:>@])],
            [gvs_storagedir="$withval"], [gvs_storagedir='${localstatedir}/lib/gnome-voice-server'])
AC_SUBST([gvs_storagedir])

AC_CONFIG_FILES([
Makefile
gnome-voice-server.spec
])
AC_OUTPUT

echo "
  ${PACKAGE_NAME} ${PACKAGE_VERSION}

    prefix:           ${prefix}
    storage dir:      ${gvs_storagedir}
    systemd units:    ${with_systemdsystemunitdir}
    compiler:         ${CC} ${CFLAGS} ${WARN_CFLAGS}
"
