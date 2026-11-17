TEST := write_data_interleave
COMPUTE_SHADERS := copy slow_copy

include ../common/guest.mk

all: $(OUT)/forbid-log.txt

$(OUT)/forbid-log.txt: $(OUT)/%: % | $(OUT)
	cp "$<" "$@"
