TEST := present_transfer
COMPUTE_SHADERS :=
LIBS := -lSceVideoOut

include ../common/guest.mk

$(OBJ): src/patches.h

src/patches.h: tools/patches.py
	python3 "$<" --header > "$@"

all: $(OUT)/config.json

$(OUT)/config.json: config.json | $(OUT)
	cp "$<" "$@"
