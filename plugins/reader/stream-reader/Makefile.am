#
# starter-configurable-service - Makefile.am
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

plugindir = $(libdir)/starter/configurable-service/plugins
plugin_LTLIBRARIES = stream-reader.la

stream_reader_la_SOURCES = stream-reader.c

stream_reader_la_CPPFLAGS = \
	$(AM_CPPFLAGS) $(STCS_CFLAGS) \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/include/starter/configurable-service

stream_reader_la_LIBADD = \
	$(top_builddir)/lib/libstarter-configurable-service.la \
	$(STCS_LIBS)

stream_reader_la_LDFLAGS = -module -avoid-version -no-undefined
