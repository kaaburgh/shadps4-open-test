TEST := hle_trophy_lifecycle
COMPUTE_SHADERS :=
LIBS := -lSceUserService -lSceNpTrophy

include ../common/guest.mk

all: $(OUT)/sce_sys/npbind.dat $(OUT)/config.json

$(OUT)/sce_sys/npbind.dat: $(COMMON)/gen_trophy_app.py | $(OUT)
	python3 "$<" "$(OUT)"

$(OUT)/config.json: config.json | $(OUT)
	cp "$<" "$@"
