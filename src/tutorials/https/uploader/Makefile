#
# Copyright (C) 2010, Strawberry Development Group
#
# This file is part of the CapROS Operating System.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2,
# or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

CROSS_BUILD=yes

EROS_SRC=../../..
include $(EROS_SRC)/build/make/makevars.mk

IMGMAP=uploader.map
GENERATED=$(BUILDDIR)/constituents.h

TARGETS=$(BUILDDIR)/uploaderResource
TARGETS+=$(BUILDDIR)/uploaderHttp
INC=-I$(BUILDDIR) -I$(EROS_ROOT)/include -I.

include $(EROS_SRC)/build/make/makerules.mk

install all: $(TARGETS)

uploaderHttp.c: $(BUILDDIR)/constituents.h

$(BUILDDIR)/uploaderResource: $(BUILDDIR)/uploaderResource.o $(LIBDEP)
	$(CROSSLINK) $(BUILDDIR)/uploaderResource.o $(CROSSLIBS) -o $@
	$(TARGET_OBJDUMP) --disassemble $@ >$(BUILDDIR)/disassembledResource

$(BUILDDIR)/uploaderHttp: $(BUILDDIR)/uploaderHttp.o $(LIBDEP)
	$(CROSSLINK) $(BUILDDIR)/uploaderHttp.o $(SMALL_SPACE) $(CROSSLIBS) -o $@
	$(TARGET_OBJDUMP) --disassemble $@ >$(BUILDDIR)/disassembled
	$(TARGET_OBJDUMP) --syms $@ | sort >$(BUILDDIR)/syms

install: all
	$(INSTALL) -d $(CAPROS_DOMAIN)
	$(INSTALL) -m 755 $(TARGETS) $(CAPROS_DOMAIN)
	$(INSTALL) -m 644 uploader.map $(CAPROS_DOMAIN)

-include $(BUILDDIR)/.*.m
