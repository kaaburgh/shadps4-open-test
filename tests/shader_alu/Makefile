TEST := shader_alu
COMPUTE_SHADERS := int_v int_s f32_v f32_s cvt_v cvt_s f64_v f64_s i64_v i64_s lds f64_pack

include ../common/guest.mk

# Both variants of a family include its operation list.
$(BUILD)/int_v.comp.spv $(BUILD)/int_s.comp.spv: assets/ops_int.glsl
$(BUILD)/f32_v.comp.spv $(BUILD)/f32_s.comp.spv: assets/ops_f32.glsl
$(BUILD)/cvt_v.comp.spv $(BUILD)/cvt_s.comp.spv: assets/ops_cvt.glsl
$(BUILD)/f64_v.comp.spv $(BUILD)/f64_s.comp.spv: assets/ops_f64.glsl
$(BUILD)/i64_v.comp.spv $(BUILD)/i64_s.comp.spv: assets/ops_i64.glsl
