#
# libstarter-configurable-service - Makefile.am
#
# Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, see <http://www.gnu.org/licenses/>.

lib_LTLIBRARIES = libstarter-configurable-service.la

libstarter_configurable_service_la_SOURCES = \
	plugin.c \
	transport.c \
	reader.c \
	writer.c \
	transform.c \
	datapoint.c \
	cron.c \
	text-to-value.c \
	date-text-to-value.c \
	map-text-to-value.c \
	value-to-text.c \
	value-to-date-text.c \
	value-to-map-text.c

libstarter_configurable_service_la_CPPFLAGS = \
	$(AM_CPPFLAGS) $(STCS_CFLAGS) \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/include/starter/configurable-service \
	-I$(top_srcdir)/src

libstarter_configurable_service_la_LIBADD = $(STCS_LIBS)
libstarter_configurable_service_la_LDFLAGS = -version-info 0:0:0
