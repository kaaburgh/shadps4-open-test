ROOT := $(abspath ../..)
DEPS := $(ROOT)/.deps
TOOLCHAIN := $(DEPS)/openorbis
OPENGNM := $(DEPS)/opengnm
PSBC := $(DEPS)/opengnm-psbc/opengnm-psbc

BUILD := $(ROOT)/build/gpu_solid_rt
OUT := $(ROOT)/out/gpu_solid_rt

ifeq ($(origin CC), default)
CC := clang
endif
ifeq ($(origin LD), default)
LD := ld.lld
endif
GLSLC ?= glslc

ELF := $(OUT)/gpu_solid_rt.elf
RAW_ELF := $(BUILD)/gpu_solid_rt.raw.elf
OELF := $(BUILD)/gpu_solid_rt.oelf
CREATE_FSELF := $(TOOLCHAIN)/bin/linux/create-fself
OBJ := $(BUILD)/main.o
VERT_SPV := $(BUILD)/fullscreen.vert.spv
FRAG_SPV := $(BUILD)/solid.frag.spv
VERT_SB := $(OUT)/assets/fullscreen.vert.sb
FRAG_SB := $(OUT)/assets/solid.frag.sb

CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
	--target=x86_64-ps4-elf -fPIC \
	-isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
	-I$(OPENGNM)/include

LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x \
	--eh-frame-hdr -L$(TOOLCHAIN)/lib

.PHONY: all clean check-deps

all: check-deps $(ELF) $(VERT_SB) $(FRAG_SB)

check-deps:
	@test -f "$(TOOLCHAIN)/link.x" || { echo "missing OpenOrbis; run ./scripts/bootstrap-deps.sh" >&2; exit 2; }
	@test -f "$(OPENGNM)/libopengnm.a" || { echo "missing libopengnm.a; run ./scripts/bootstrap-deps.sh" >&2; exit 2; }
	@test -x "$(PSBC)" || { echo "missing opengnm-psbc; run ./scripts/bootstrap-deps.sh" >&2; exit 2; }
	@command -v "$(GLSLC)" >/dev/null 2>&1 || { echo "missing glslc" >&2; exit 2; }

$(BUILD) $(OUT) $(OUT)/assets:
	@mkdir -p "$@"

$(OBJ): src/main.c | $(BUILD)
	$(CC) $(CFLAGS) -c "$<" -o "$@"

# ld.lld emits a plain ET_DYN/SysV ELF; the PS4 loader (and shadPS4) needs the
# FreeBSD-ABI ET_SCE_* image with PT_SCE_* segments that create-fself produces.
$(ELF): $(RAW_ELF) | $(OUT)
	OO_PS4_TOOLCHAIN="$(TOOLCHAIN)" "$(CREATE_FSELF)" -in="$<" -out="$(OELF)" \
		--eboot "$@" --paid 0x3800000000000011

$(RAW_ELF): $(OBJ) $(OPENGNM)/libopengnm.a | $(BUILD)
	$(LD) -o "$@" "$(OBJ)" $(LDFLAGS) \
		"$(OPENGNM)/libopengnm.a" -lc -lkernel -lSceGnmDriver -lSceVideoOut \
		"$(TOOLCHAIN)/lib/crt1.o" "$(TOOLCHAIN)/lib/crti.o" "$(TOOLCHAIN)/lib/crtn.o"

$(VERT_SPV): assets/fullscreen.vert.glsl | $(BUILD)
	$(GLSLC) -fshader-stage=vertex "$<" -o "$@"

$(FRAG_SPV): assets/solid.frag.glsl | $(BUILD)
	$(GLSLC) -fshader-stage=fragment "$<" -o "$@"

$(VERT_SB): $(VERT_SPV) | $(OUT)/assets
	"$(PSBC)" -s vertex -f "$<" -o "$@" -4

$(FRAG_SB): $(FRAG_SPV) | $(OUT)/assets
	"$(PSBC)" -s fragment -f "$<" -o "$@" -4

clean:
	rm -rf "$(BUILD)" "$(OUT)"
