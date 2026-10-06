ROOT := $(abspath ../..)
DEPS := $(ROOT)/.deps
TOOLCHAIN := $(DEPS)/openorbis
OPENGNM := $(DEPS)/opengnm
PSBC := $(DEPS)/opengnm-psbc/opengnm-psbc

BUILD := $(ROOT)/build/buffer_compute_roundtrip
OUT := $(ROOT)/out/buffer_compute_roundtrip

ifeq ($(origin CC), default)
CC := clang
endif
ifeq ($(origin LD), default)
LD := ld.lld
endif
GLSLC ?= glslc

ELF := $(OUT)/buffer_compute_roundtrip.elf
RAW_ELF := $(BUILD)/buffer_compute_roundtrip.raw.elf
OELF := $(BUILD)/buffer_compute_roundtrip.oelf
CREATE_FSELF := $(TOOLCHAIN)/bin/linux/create-fself
OBJ := $(BUILD)/main.o
COMP_SPV := $(BUILD)/roundtrip.comp.spv
COMP_SB := $(OUT)/assets/roundtrip.comp.sb

CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
	--target=x86_64-ps4-elf -fPIC \
	-isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
	-I$(OPENGNM)/include

LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x \
	--eh-frame-hdr -L$(TOOLCHAIN)/lib

.PHONY: all clean check-deps

all: check-deps $(ELF) $(COMP_SB)

check-deps:
	@test -f "$(TOOLCHAIN)/link.x" || { echo "missing OpenOrbis; run ./scripts/bootstrap-deps.sh" >&2; exit 2; }
	@test -f "$(OPENGNM)/libopengnm.a" || { echo "missing libopengnm.a; run ./scripts/bootstrap-deps.sh" >&2; exit 2; }
	@test -x "$(PSBC)" || { echo "missing opengnm-psbc; run ./scripts/bootstrap-deps.sh" >&2; exit 2; }
	@command -v "$(GLSLC)" >/dev/null 2>&1 || { echo "missing glslc" >&2; exit 2; }

$(BUILD) $(OUT) $(OUT)/assets:
	@mkdir -p "$@"

$(OBJ): src/main.c | $(BUILD)
	$(CC) $(CFLAGS) -c "$<" -o "$@"

$(ELF): $(RAW_ELF) | $(OUT)
	OO_PS4_TOOLCHAIN="$(TOOLCHAIN)" "$(CREATE_FSELF)" -in="$<" -out="$(OELF)" \
		--eboot "$@" --paid 0x3800000000000011

$(RAW_ELF): $(OBJ) $(OPENGNM)/libopengnm.a | $(BUILD)
	$(LD) -o "$@" "$(OBJ)" $(LDFLAGS) \
		"$(OPENGNM)/libopengnm.a" -lc -lkernel -lSceGnmDriver -lSceVideoOut \
		"$(TOOLCHAIN)/lib/crt1.o" "$(TOOLCHAIN)/lib/crti.o" "$(TOOLCHAIN)/lib/crtn.o"

$(COMP_SPV): assets/roundtrip.comp.glsl | $(BUILD)
	$(GLSLC) -fshader-stage=compute --target-env=vulkan1.1 "$<" -o "$@"

$(COMP_SB): $(COMP_SPV) | $(OUT)/assets
	"$(PSBC)" -s compute -f "$<" -o "$@" -4

clean:
	rm -rf "$(BUILD)" "$(OUT)"
