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

bin_PROGRAMS = current-time

current_time_SOURCES = current-time.c

current_time_CPPFLAGS = \
	$(AM_CPPFLAGS) $(GLIB_CFLAGS)

current_time_LDADD = \
	$(GLIB_LIBS)

exampledir = $(datadir)/$(PACKAGE)/examples/current-time
nodist_example_DATA = current-time.stcs.xml

current-time.stcs.xml: current-time.stcs.xml.in
	$(AM_V_GEN)sed -e 's|[@]bindir[@]|$(bindir)|g' \
	    < $< > $@-t && mv $@-t $@

all-local: current-time.stcs.xml

CLEANFILES = current-time.stcs.xml

EXTRA_DIST = current-time.stcs.xml.in
