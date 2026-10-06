# Shared build rules for compute/buffer guest workloads.
#
# A test's Makefile sets
#   TEST            the test name (its directory under tests/)
#   COMPUTE_SHADERS basenames of assets/<name>.comp.glsl to compile with
#                   opengnm-psbc; each lands at out/<test>/assets/<name>.comp.sb
# and then includes this file. src/main.c is the only guest source.

COMMON := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# build-test.sh and run-test.py find a test by its directory name, and an
# empty TEST would make `clean` remove every test's output.
ifneq ($(TEST),$(notdir $(CURDIR)))
$(error TEST ($(TEST)) must match the test directory name ($(notdir $(CURDIR))))
endif

ROOT := $(abspath $(COMMON)/../..)
DEPS := $(ROOT)/.deps
TOOLCHAIN := $(DEPS)/openorbis
OPENGNM := $(DEPS)/opengnm
PSBC := $(DEPS)/opengnm-psbc/opengnm-psbc

BUILD := $(ROOT)/build/$(TEST)
OUT := $(ROOT)/out/$(TEST)

ifeq ($(origin CC), default)
CC := clang
endif
ifeq ($(origin LD), default)
LD := ld.lld
endif
GLSLC ?= glslc

ELF := $(OUT)/$(TEST).elf
RAW_ELF := $(BUILD)/$(TEST).raw.elf
OELF := $(BUILD)/$(TEST).oelf
CREATE_FSELF := $(TOOLCHAIN)/bin/linux/create-fself
OBJ := $(BUILD)/main.o
COMP_SB := $(patsubst %,$(OUT)/assets/%.comp.sb,$(COMPUTE_SHADERS))

CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g -MMD -MP \
	--target=x86_64-ps4-elf -fPIC \
	-isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
	-I$(OPENGNM)/include -I$(COMMON)

LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x \
	--eh-frame-hdr -L$(TOOLCHAIN)/lib

.PHONY: all clean check-deps
.SECONDARY:

all: check-deps $(ELF) $(COMP_SB)

check-deps:
	@test -f "$(TOOLCHAIN)/link.x" || { echo "missing OpenOrbis; run ./scripts/bootstrap-deps.sh" >&2; exit 2; }
	@test -f "$(OPENGNM)/libopengnm.a" || { echo "missing libopengnm.a; run ./scripts/bootstrap-deps.sh" >&2; exit 2; }
	@test -x "$(PSBC)" || { echo "missing opengnm-psbc; run ./scripts/bootstrap-deps.sh" >&2; exit 2; }
	@command -v "$(GLSLC)" >/dev/null 2>&1 || { echo "missing glslc" >&2; exit 2; }

$(BUILD) $(OUT) $(OUT)/assets:
	@mkdir -p "$@"

$(OBJ): src/main.c $(COMMON)/shadtest_guest.h | $(BUILD)
	$(CC) $(CFLAGS) -c "$<" -o "$@"

$(ELF): $(RAW_ELF) | $(OUT)
	OO_PS4_TOOLCHAIN="$(TOOLCHAIN)" "$(CREATE_FSELF)" -in="$<" -out="$(OELF)" \
		--eboot "$@" --paid 0x3800000000000011

$(RAW_ELF): $(OBJ) $(OPENGNM)/libopengnm.a | $(BUILD)
	$(LD) -o "$@" "$(OBJ)" $(LDFLAGS) \
		"$(OPENGNM)/libopengnm.a" -lc -lkernel -lSceGnmDriver -lSceVideoOut \
		"$(TOOLCHAIN)/lib/crt1.o" "$(TOOLCHAIN)/lib/crti.o" "$(TOOLCHAIN)/lib/crtn.o"

$(BUILD)/%.comp.spv: assets/%.comp.glsl | $(BUILD)
	$(GLSLC) -fshader-stage=compute --target-env=vulkan1.1 "$<" -o "$@"

# Depends on the compiler too: bootstrap rebuilds psbc in place when its
# patch set changes.
$(OUT)/assets/%.comp.sb: $(BUILD)/%.comp.spv $(PSBC) | $(OUT)/assets
	"$(PSBC)" -s compute -f "$<" -o "$@" -4

clean:
	rm -rf "$(BUILD)" "$(OUT)"

# Header dependencies, including OpenGNM's inline helpers.
-include $(OBJ:.o=.d)
