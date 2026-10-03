#
# starter-configurable-service - configure.ac
#
# Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
# Process this file with autoconf to produce configure.

AC_INIT([starter-configurable-service], [1.0.0], [startercontroller@gmail.com])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([src/autoconf.h])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([-Wall -Werror])

AC_PROG_CC
AM_PROG_AR

LT_INIT([dlopen])

PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.30 gobject-2.0 >= 2.30 gio-2.0 >= 2.30 gmodule-2.0])
PKG_CHECK_MODULES([STARTER], [starter-core])
PKG_CHECK_MODULES([LIBXML2], [libxml-2.0 >= 2.9.0])
PKG_CHECK_MODULES([JSON_GLIB], [json-glib-1.0])
PKG_CHECK_MODULES([SQLITE3], [sqlite3])

# GLib APIs added after Ubuntu 20.04's 2.64 (e.g. g_string_replace @ 2.68)
saved_CFLAGS=$CFLAGS
saved_LIBS=$LIBS
CFLAGS="$CFLAGS $GLIB_CFLAGS"
LIBS="$LIBS $GLIB_LIBS"
AC_CHECK_FUNCS([g_string_replace])
CFLAGS=$saved_CFLAGS
LIBS=$saved_LIBS

# Prefer libsoup-3.0, fall back to libsoup-2.4 (2.58+ covers Ubuntu 20.04 / 2.70)
have_libsoup3=no
PKG_CHECK_MODULES([LIBSOUP], [libsoup-3.0],
  [have_libsoup3=yes],
  [PKG_CHECK_MODULES([LIBSOUP], [libsoup-2.4 >= 2.58])])

AM_CONDITIONAL([HAVE_LIBSOUP_3], [test "$have_libsoup3" = "yes"])
AS_IF([test "$have_libsoup3" = "yes"],
  [AC_DEFINE([HAVE_LIBSOUP_3], [1], [Define if building against libsoup 3.x])])

STCS_CFLAGS="$GLIB_CFLAGS $STARTER_CFLAGS $LIBXML2_CFLAGS"
STCS_LIBS="$GLIB_LIBS $STARTER_LIBS $LIBXML2_LIBS"

AC_SUBST(STCS_CFLAGS)
AC_SUBST(STCS_LIBS)
AC_SUBST(JSON_GLIB_CFLAGS)
AC_SUBST(JSON_GLIB_LIBS)
AC_SUBST(SQLITE3_CFLAGS)
AC_SUBST(SQLITE3_LIBS)
AC_SUBST(LIBSOUP_CFLAGS)
AC_SUBST(LIBSOUP_LIBS)

AC_PATH_TOOL([MAKEINFO], [makeinfo], [:])
if test "$MAKEINFO" = ":" ; then
    AC_MSG_ERROR([makeinfo not found. Please install it and try again.])
fi

# =========================================================

AC_CONFIG_FILES([
  Makefile
  include/Makefile
  lib/Makefile
  src/Makefile
  plugins/Makefile
  plugins/transport/Makefile
  plugins/transport/file-transport/Makefile
  plugins/transport/http-transport/Makefile
  plugins/transport/process-transport/Makefile
  plugins/reader/Makefile
  plugins/reader/json-reader/Makefile
  plugins/reader/stream-reader/Makefile
  plugins/writer/Makefile
  plugins/writer/json-writer/Makefile
  plugins/writer/stream-writer/Makefile
  plugins/transform/Makefile
  plugins/transform/formula-transform/Makefile
  plugins/transform/sampling-transform/Makefile
  examples/Makefile
  examples/current-time/Makefile
  doc/Makefile
])

AC_OUTPUT

AS_IF([test "$have_libsoup3" = "yes"],
  [soup_msg="libsoup-3.0"],
  [soup_msg="libsoup-2.4"])
AC_MSG_NOTICE([HTTP transport will use $soup_msg])
