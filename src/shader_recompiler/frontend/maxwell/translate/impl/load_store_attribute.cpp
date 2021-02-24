// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
enum class InterpolationMode : u64 {
    Pass,
    Multiply,
    Constant,
    Sc,
};

enum class SampleMode : u64 {
    Default,
    Centroid,
    Offset,
};
} // Anonymous namespace

void TranslatorVisitor::IPA(u64 insn) {
    // IPA is the instruction used to read varyings from a fragment shader.
    // gl_FragCoord is mapped to the gl_Position attribute.
    // It yields unknown results when used outside of the fragment shader stage.
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> index_reg;
        BitField<20, 8, IR::Reg> multiplier;
        BitField<30, 8, IR::Attribute> attribute;
        BitField<38, 1, u64> idx;
        BitField<51, 1, u64> sat;
        BitField<52, 2, SampleMode> sample_mode;
        BitField<54, 2, InterpolationMode> interpolation_mode;
    } const ipa{insn};

    // Indexed IPAs are used for indexed varyings.
    // For example:
    //
    // in vec4 colors[4];
    // uniform int idx;
    // void main() {
    //     gl_FragColor = colors[idx];
    // }
    const bool is_indexed{ipa.idx != 0 && ipa.index_reg != IR::Reg::RZ};
    if (is_indexed) {
        throw NotImplementedException("IPA.IDX");
    }

    const IR::Attribute attribute{ipa.attribute};
    IR::F32 value{ir.GetAttribute(attribute)};
    if (IR::IsGeneric(attribute)) {
        // const bool is_perspective{UnimplementedReadHeader(GenericAttributeIndex(attribute))};
        const bool is_perspective{false};
        if (is_perspective) {
            const IR::F32 rcp_position_w{ir.FPRecip(ir.GetAttribute(IR::Attribute::PositionW))};
            value = ir.FPMul(value, rcp_position_w);
        }
    }

    switch (ipa.interpolation_mode) {
    case InterpolationMode::Pass:
        break;
    case InterpolationMode::Multiply:
        value = ir.FPMul(value, F(ipa.multiplier));
        break;
    case InterpolationMode::Constant:
        throw NotImplementedException("IPA.CONSTANT");
    case InterpolationMode::Sc:
        throw NotImplementedException("IPA.SC");
    }

    // Saturated IPAs are generally generated out of clamped varyings.
    // For example: clamp(some_varying, 0.0, 1.0)
    const bool is_saturated{ipa.sat != 0};
    if (is_saturated) {
        if (attribute == IR::Attribute::FrontFace) {
            throw NotImplementedException("IPA.SAT on FrontFace");
        }
        value = ir.FPSaturate(value);
    }

    F(ipa.dest_reg, value);
}

} // namespace Shader::Maxwell