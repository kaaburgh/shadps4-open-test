TEST := hle_trophy_metadata
COMPUTE_SHADERS :=
LIBS := -lSceUserService -lSceNpTrophy

include ../common/guest.mk

all: $(OUT)/sce_sys/npbind.dat

$(OUT)/sce_sys/npbind.dat: $(COMMON)/gen_trophy_app.py | $(OUT)
	python3 "$<" "$(OUT)"
