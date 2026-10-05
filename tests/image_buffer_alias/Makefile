TEST := image_buffer_alias
COMPUTE_SHADERS := transform

include ../common/guest.mk

# Graphics shaders: shared rules only cover compute.
GRAPHICS_SB := $(OUT)/assets/fullscreen.vert.sb $(OUT)/assets/pattern.frag.sb \
	$(OUT)/assets/red.frag.sb

all: $(GRAPHICS_SB)

$(BUILD)/%.vert.spv: assets/%.vert.glsl | $(BUILD)
	$(GLSLC) -fshader-stage=vertex --target-env=vulkan1.1 "$<" -o "$@"

$(BUILD)/%.frag.spv: assets/%.frag.glsl | $(BUILD)
	$(GLSLC) -fshader-stage=fragment --target-env=vulkan1.1 "$<" -o "$@"

$(OUT)/assets/%.vert.sb: $(BUILD)/%.vert.spv | $(OUT)/assets
	"$(PSBC)" -s vertex -f "$<" -o "$@" -4

$(OUT)/assets/%.frag.sb: $(BUILD)/%.frag.spv | $(OUT)/assets
	"$(PSBC)" -s fragment -f "$<" -o "$@" -4
