TEST := hle_trophy_popup
COMPUTE_SHADERS :=
LIBS := -lSceUserService -lSceNpTrophy

include ../common/guest.mk

all: $(OUT)/sce_sys/npbind.dat $(OUT)/config.json $(OUT)/forbid-log.txt

$(OUT)/sce_sys/npbind.dat: $(COMMON)/gen_trophy_app.py | $(OUT)
	python3 "$<" "$(OUT)"

$(OUT)/config.json $(OUT)/forbid-log.txt: $(OUT)/%: % | $(OUT)
	cp "$<" "$@"
