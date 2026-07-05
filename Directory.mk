############################################################################
# apps/Directory.mk
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(APPDIR)/Make.defs

# Sub-directories that have been built or configured.

SUBDIRS       := $(dir $(wildcard */Makefile))
CONFIGSUBDIRS := $(filter-out $(dir $(wildcard */Kconfig)),$(SUBDIRS))
ifeq ($(CONFIG_WINDOWS_NATIVE),y)
  CONFIGSUBDIRS := $(subst /,\\,$(CONFIGSUBDIRS))
  SUBDIRS       := $(subst /,\\,$(SUBDIRS))
endif

all: nothing

.PHONY: nothing clean distclean

$(foreach SDIR, $(CONFIGSUBDIRS), $(eval $(call SDIR_template,$(SDIR),preconfig)))

# clean and distclean both use SUBDIRS (all directories with Makefiles).
# clean on un-built directories is harmless (no-op).  distclean can reach
# directories whose context:: target created build artefacts even when the
# build failed before .built or .depend markers were written.
#
# The '-' prefix on the recursive make call allows clean/distclean to
# continue even when individual subdirectories fail (e.g. missing tools
# like cargo).  TOPDIR is passed so that sub-makes can include
# $(TOPDIR)/Make.defs and define SDIR_template for further recursion.

$(foreach SDIR, $(SUBDIRS), $(eval .PHONY: $(SDIR)_clean))
$(foreach SDIR, $(SUBDIRS), $(eval $(SDIR)_clean: ; $$(Q) $$(MAKE) -C $(SDIR) clean APPDIR="$$(APPDIR)" TOPDIR="$$(TOPDIR)"))

$(foreach SDIR, $(SUBDIRS), $(eval .PHONY: $(SDIR)_distclean))
$(foreach SDIR, $(SUBDIRS), $(eval $(SDIR)_distclean: ; +-$$(Q) $$(MAKE) -C $(SDIR) distclean APPDIR="$$(APPDIR)" TOPDIR="$$(TOPDIR)"))

nothing:

install:

preconfig: $(foreach SDIR, $(CONFIGSUBDIRS), $(SDIR)_preconfig)
ifneq ($(MENUDESC),)
	$(Q) $(MKKCONFIG) -m $(MENUDESC)
endif
	$(Q) touch .kconfig

clean: $(foreach SDIR, $(SUBDIRS), $(SDIR)_clean)
	@:

distclean: $(foreach SDIR, $(SUBDIRS), $(SDIR)_distclean)
ifneq ($(MENUDESC),)
	$(call DELFILE, Kconfig)
endif
	$(call DELFILE, .kconfig)

-include Make.dep
