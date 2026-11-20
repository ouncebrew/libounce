#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

include $(DEVKITPRO)/devkitA64/base_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
#---------------------------------------------------------------------------------
TARGET		:=	nx
#BUILD		:=	build
SOURCES		:=	source/arm source/kernel source/sf source/services source/crypto source/nvidia source/nvidia/ioctl source/display source/audio source/applets source/runtime source/runtime/devices source/runtime/util source/runtime/util/utf
DATA		:=	data
INCLUDES	:=	include external/bsd/include

ifeq ($(SWITCH2),1)
TARGET		:=	ounce
SOURCES		+=	source/switch2/gameshare source/switch2/services source/switch2/kernel source/switch2/display source/switch2/video source/switch2/memory
# MALLOC=1 builds libounce_malloc.a, the mimalloc allocator, on its own;
# the rest of the library is libounce_core.a.
ifeq ($(MALLOC),1)
SOURCES		:=	source/switch2/malloc
DATA		:=
INCLUDES	+=	external/mimalloc/include
endif
# MALLOC_INIT=1 builds libounce_malloc_init.a: __libnx_init with a strong
# reference to the allocator, which takes the place of init.o in libounce.a.
ifeq ($(MALLOC_INIT),1)
SOURCES		:=	source/switch2/malloc_init
DATA		:=
endif
endif

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ifeq ($(SWITCH2),1)
# The Switch 2's CPU, as switch2_rules gives it to programs.
include $(dir $(abspath $(firstword $(MAKEFILE_LIST))))switch2_cpu.mk
ARCH	:=	$(SWITCH2_CPU) -mtp=soft -fPIC -ftls-model=local-exec
else
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec
endif

CFLAGS	:=	-g -Wall -Werror \
			-ffunction-sections \
			-fdata-sections \
			$(ARCH) \
			$(BUILD_CFLAGS)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -DLIBNX_NO_DEPRECATION

ifeq ($(SWITCH2),1)
CFLAGS	+=	-D__SWITCH2__
endif

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)

ifeq ($(SWITCH2),1)
ASFLAGS	+=	-D__SWITCH2__
endif

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I. \
			-iquote $(CURDIR)/include/switch/

.PHONY: clean all switch1 switch2 install install-switch1 install-switch2 dist-src lib/libnx.a lib/libnxd.a lib/libounce.a lib/libounced.a \
	lib/libounce_core.a lib/libounce_cored.a lib/libounce_malloc.a lib/libounce_mallocd.a

#---------------------------------------------------------------------------------
# The Switch 2 library is the default; libnx for Switch 1 builds from the same
# sources with switch1 and install-switch1.
all: switch2

switch2:
	@$(MAKE) SWITCH2=1 lib/libounce.a lib/libounced.a

switch1: lib/libnx.a lib/libnxd.a

install: install-switch2

install-switch1: lib/libnx.a lib/libnxd.a
	@mkdir -p $(DESTDIR)$(DEVKITPRO)/libnx
	@cp -v default_icon.jpg switch_rules switch.ld switch.specs $(DESTDIR)$(DEVKITPRO)/libnx/
	@cp -rv include lib $(DESTDIR)$(DEVKITPRO)/libnx/
	@cp -rv external/bsd/include $(DESTDIR)$(DEVKITPRO)/libnx/

# Installs the Switch 2 library beside libnx, in $(DEVKITPRO)/libounce. The
# files lib/libnx.a and lib/libnxd.a are linker scripts that select libounce,
# so a Makefile that links -lnx links libounce.
install-switch2: switch2
	@mkdir -p $(DESTDIR)$(DEVKITPRO)/libounce/lib
	@cp -v default_icon.jpg switch_rules switch2_rules switch2_cpu.mk switch.ld $(DESTDIR)$(DEVKITPRO)/libounce/
	@sed 's|/libnx/switch.ld|/libounce/switch.ld|' switch.specs > $(DESTDIR)$(DEVKITPRO)/libounce/switch.specs
	@cp -rv include $(DESTDIR)$(DEVKITPRO)/libounce/
	@printf '%s\n' '/**' ' * @file target.h' ' * @brief The platform that these headers are installed for: Switch 2.' \
		' * @note Installed by make install-switch2: a program that includes these headers' \
		' * is built for Switch 2 whatever its compiler flags.' ' */' '#pragma once' '' \
		'#ifndef __SWITCH2__' '#define __SWITCH2__ 1' '#endif' > $(DESTDIR)$(DEVKITPRO)/libounce/include/switch/target.h
	@cp -rv external/bsd/include $(DESTDIR)$(DEVKITPRO)/libounce/
	@cp -v lib/libounce.a lib/libounced.a lib/libounce_core.a lib/libounce_cored.a \
		lib/libounce_malloc.a lib/libounce_mallocd.a $(DESTDIR)$(DEVKITPRO)/libounce/lib/
	@echo 'INPUT(libounce.a)' > $(DESTDIR)$(DEVKITPRO)/libounce/lib/libnx.a
	@echo 'INPUT(libounced.a)' > $(DESTDIR)$(DEVKITPRO)/libounce/lib/libnxd.a
	@mkdir -p $(DESTDIR)$(DEVKITPRO)/libounce/cmake/Platform $(DESTDIR)$(DEVKITPRO)/portlibs/switch2/bin
	@cp -v ../toolchain/switch2vars.sh $(DESTDIR)$(DEVKITPRO)/
	@cp -v ../toolchain/cmake/Switch2.cmake $(DESTDIR)$(DEVKITPRO)/libounce/cmake/
	@cp -v ../toolchain/cmake/Platform/NintendoSwitch.cmake $(DESTDIR)$(DEVKITPRO)/libounce/cmake/Platform/
	@cp -v ../toolchain/aarch64-none-elf-cmake $(DESTDIR)$(DEVKITPRO)/portlibs/switch2/bin/
	@mkdir -p $(DESTDIR)$(DEVKITPRO)/libounce/licenses
	@cp -v ../LICENSE.md $(DESTDIR)$(DEVKITPRO)/libounce/licenses/libnx-ISC.txt
	@cp -v ../LICENSES/libounce-ISC.txt $(DESTDIR)$(DEVKITPRO)/libounce/licenses/
	@cp -v external/mimalloc/LICENSE $(DESTDIR)$(DEVKITPRO)/libounce/licenses/mimalloc-MIT.txt

# The repository at this commit, as libounce-<commit>.tar.gz.
dist-src:
	@revision=$$(git rev-parse --short HEAD) && \
	git -C .. archive --prefix=libounce-$$revision/ -o $(CURDIR)/libounce-$$revision.tar.gz HEAD && \
	echo libounce-$$revision.tar.gz

#dox:
#	@doxygen Doxyfile
#	@doxygen Doxyfile.internal

lib:
	@[ -d $@ ] || mkdir -p $@

release:
	@[ -d $@ ] || mkdir -p $@

debug:
	@[ -d $@ ] || mkdir -p $@

lib/libnx.a : lib release $(SOURCES) $(INCLUDES)
	@$(MAKE) BUILD=release OUTPUT=$(CURDIR)/$@ \
	BUILD_CFLAGS="-DNDEBUG=1 -O2" \
	DEPSDIR=$(CURDIR)/release \
	--no-print-directory -C release \
	-f $(CURDIR)/Makefile

lib/libnxd.a : lib debug $(SOURCES) $(INCLUDES)
	@$(MAKE) BUILD=debug OUTPUT=$(CURDIR)/$@ \
	BUILD_CFLAGS="-DDEBUG=1 -Og" \
	DEPSDIR=$(CURDIR)/debug \
	--no-print-directory -C debug \
	-f $(CURDIR)/Makefile

release-switch2:
	@[ -d $@ ] || mkdir -p $@

debug-switch2:
	@[ -d $@ ] || mkdir -p $@

release-switch2-malloc:
	@[ -d $@ ] || mkdir -p $@

debug-switch2-malloc:
	@[ -d $@ ] || mkdir -p $@

release-switch2-malloc-init:
	@[ -d $@ ] || mkdir -p $@

debug-switch2-malloc-init:
	@[ -d $@ ] || mkdir -p $@

# mimalloc's own functions and data (mi_*, _mi_*, __mi_*) become local
# symbols of malloc.o: nothing else in libounce uses them, and a program that
# bundles its own mimalloc then links without multiple definitions. Only the
# allocator entry points of switch2/malloc/malloc.c stay global.
define localize_mimalloc
	@$(PREFIX)objcopy --wildcard --localize-symbol='mi_*' \
		--localize-symbol='_mi_*' --localize-symbol='__mi_*' $(1)
endef

# libounce.a is the whole library, one ordinary archive: libounce_core.a with
# libounce_malloc_init.a's __libnx_init in place of init.o, and the mimalloc
# allocator (libounce_malloc.a) in place of heap.o, whose heap figures are
# newlib's. The strong reference in that __libnx_init brings the allocator
# in, before anything can call newlib's malloc. A small native process that
# keeps newlib's malloc links -lounce_core instead.
# $(1): the output, $(2): the core, $(3): the allocator, $(4): its init.
define merge_ounce
	@rm -f $(1) $(1).tmp
	@printf 'create %s\naddlib %s\ndelete init.o\ndelete heap.o\naddlib %s\naddlib %s\nsave\nend\n' \
		$(1).tmp $(2) $(4) $(3) | $(AR) -M
	@$(AR) s $(1).tmp
	@mv $(1).tmp $(1)
endef

lib/libounce.a : lib/libounce_core.a | lib
	@$(MAKE) --no-print-directory SWITCH2=1 MALLOC=1 lib/libounce_malloc.a
	@$(MAKE) --no-print-directory SWITCH2=1 MALLOC_INIT=1 lib/libounce_malloc_init.a
	$(call merge_ounce,$@,lib/libounce_core.a,lib/libounce_malloc.a,lib/libounce_malloc_init.a)

lib/libounced.a : lib/libounce_cored.a | lib
	@$(MAKE) --no-print-directory SWITCH2=1 MALLOC=1 lib/libounce_mallocd.a
	@$(MAKE) --no-print-directory SWITCH2=1 MALLOC_INIT=1 lib/libounce_malloc_initd.a
	$(call merge_ounce,$@,lib/libounce_cored.a,lib/libounce_mallocd.a,lib/libounce_malloc_initd.a)

lib/libounce_core.a : lib release-switch2 $(SOURCES) $(INCLUDES)
	@$(MAKE) BUILD=release-switch2 SWITCH2=1 OUTPUT=$(CURDIR)/$@ \
	BUILD_CFLAGS="-DNDEBUG=1 -O2" \
	DEPSDIR=$(CURDIR)/release-switch2 \
	--no-print-directory -C release-switch2 \
	-f $(CURDIR)/Makefile

lib/libounce_cored.a : lib debug-switch2 $(SOURCES) $(INCLUDES)
	@$(MAKE) BUILD=debug-switch2 SWITCH2=1 OUTPUT=$(CURDIR)/$@ \
	BUILD_CFLAGS="-DDEBUG=1 -Og" \
	DEPSDIR=$(CURDIR)/debug-switch2 \
	--no-print-directory -C debug-switch2 \
	-f $(CURDIR)/Makefile

lib/libounce_malloc.a : lib release-switch2-malloc $(SOURCES) $(INCLUDES)
	@$(MAKE) BUILD=release-switch2-malloc SWITCH2=1 MALLOC=1 OUTPUT=$(CURDIR)/$@ \
	BUILD_CFLAGS="-DNDEBUG=1 -O2" \
	DEPSDIR=$(CURDIR)/release-switch2-malloc \
	--no-print-directory -C release-switch2-malloc \
	-f $(CURDIR)/Makefile
	$(call localize_mimalloc,$@)

lib/libounce_mallocd.a : lib debug-switch2-malloc $(SOURCES) $(INCLUDES)
	@$(MAKE) BUILD=debug-switch2-malloc SWITCH2=1 MALLOC=1 OUTPUT=$(CURDIR)/$@ \
	BUILD_CFLAGS="-DDEBUG=1 -Og" \
	DEPSDIR=$(CURDIR)/debug-switch2-malloc \
	--no-print-directory -C debug-switch2-malloc \
	-f $(CURDIR)/Makefile
	$(call localize_mimalloc,$@)

lib/libounce_malloc_init.a : lib release-switch2-malloc-init $(SOURCES) $(INCLUDES)
	@$(MAKE) BUILD=release-switch2-malloc-init SWITCH2=1 MALLOC_INIT=1 OUTPUT=$(CURDIR)/$@ \
	BUILD_CFLAGS="-DNDEBUG=1 -O2" \
	DEPSDIR=$(CURDIR)/release-switch2-malloc-init \
	--no-print-directory -C release-switch2-malloc-init \
	-f $(CURDIR)/Makefile

lib/libounce_malloc_initd.a : lib debug-switch2-malloc-init $(SOURCES) $(INCLUDES)
	@$(MAKE) BUILD=debug-switch2-malloc-init SWITCH2=1 MALLOC_INIT=1 OUTPUT=$(CURDIR)/$@ \
	BUILD_CFLAGS="-DDEBUG=1 -Og" \
	DEPSDIR=$(CURDIR)/debug-switch2-malloc-init \
	--no-print-directory -C debug-switch2-malloc-init \
	-f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr release debug release-switch2 debug-switch2 release-switch2-malloc debug-switch2-malloc release-switch2-malloc-init debug-switch2-malloc-init lib docs internal_docs

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT)	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES)

#---------------------------------------------------------------------------------
%_bin.h %.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)


-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------

