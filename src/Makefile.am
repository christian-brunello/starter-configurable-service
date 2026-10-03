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

bin_PROGRAMS = starter-configurable-service

starter_configurable_service_SOURCES = \
	main.c \
	internals.c \
	internals.h \
	config.c \
	runlevel.c \
	log.c \
	plugin-entry.c \
	plugin-entry.h \
	glib-compat.c \
	glib-compat.h

starter_configurable_service_CPPFLAGS = \
	$(AM_CPPFLAGS) $(STCS_CFLAGS) \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/include/starter/configurable-service

starter_configurable_service_LDADD = \
	$(STCS_LIBS) \
	$(top_builddir)/lib/libstarter-configurable-service.la

starter_configurable_service_LDFLAGS = -export-dynamic
