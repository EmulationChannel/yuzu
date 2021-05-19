// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <utility>

#include "shader_recompiler/backend/glasm/emit_context.h"
#include "shader_recompiler/backend/glasm/emit_glasm_instructions.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::Backend::GLASM {
namespace {
struct ScopedRegister {
    ScopedRegister() = default;
    ScopedRegister(RegAlloc& reg_alloc_) : reg_alloc{&reg_alloc_}, reg{reg_alloc->AllocReg()} {}

    ~ScopedRegister() {
        if (reg_alloc) {
            reg_alloc->FreeReg(reg);
        }
    }

    ScopedRegister& operator=(ScopedRegister&& rhs) noexcept {
        if (reg_alloc) {
            reg_alloc->FreeReg(reg);
        }
        reg_alloc = std::exchange(rhs.reg_alloc, nullptr);
        reg = rhs.reg;
        return *this;
    }

    ScopedRegister(ScopedRegister&& rhs) noexcept
        : reg_alloc{std::exchange(rhs.reg_alloc, nullptr)}, reg{rhs.reg} {}

    ScopedRegister& operator=(const ScopedRegister&) = delete;
    ScopedRegister(const ScopedRegister&) = delete;

    RegAlloc* reg_alloc{};
    Register reg;
};

std::string Texture(EmitContext& ctx, IR::TextureInstInfo info,
                    [[maybe_unused]] const IR::Value& index) {
    // FIXME: indexed reads
    if (info.type == TextureType::Buffer) {
        return fmt::format("texture[{}]", ctx.texture_buffer_bindings.at(info.descriptor_index));
    } else {
        return fmt::format("texture[{}]", ctx.texture_bindings.at(info.descriptor_index));
    }
}

std::string_view TextureType(IR::TextureInstInfo info) {
    if (info.is_depth) {
        switch (info.type) {
        case TextureType::Color1D:
            return "SHADOW1D";
        case TextureType::ColorArray1D:
            return "SHADOWARRAY1D";
        case TextureType::Color2D:
            return "SHADOW2D";
        case TextureType::ColorArray2D:
            return "SHADOWARRAY2D";
        case TextureType::Color3D:
            return "SHADOW3D";
        case TextureType::ColorCube:
            return "SHADOWCUBE";
        case TextureType::ColorArrayCube:
            return "SHADOWARRAYCUBE";
        case TextureType::Buffer:
            return "SHADOWBUFFER";
        }
    } else {
        switch (info.type) {
        case TextureType::Color1D:
            return "1D";
        case TextureType::ColorArray1D:
            return "ARRAY1D";
        case TextureType::Color2D:
            return "2D";
        case TextureType::ColorArray2D:
            return "ARRAY2D";
        case TextureType::Color3D:
            return "3D";
        case TextureType::ColorCube:
            return "CUBE";
        case TextureType::ColorArrayCube:
            return "ARRAYCUBE";
        case TextureType::Buffer:
            return "BUFFER";
        }
    }
    throw InvalidArgument("Invalid texture type {}", info.type.Value());
}

std::string Offset(EmitContext& ctx, const IR::Value& offset) {
    if (offset.IsEmpty()) {
        return "";
    }
    return fmt::format(",offset({})", Register{ctx.reg_alloc.Consume(offset)});
}

std::pair<ScopedRegister, ScopedRegister> AllocOffsetsRegs(EmitContext& ctx,
                                                           const IR::Value& offset2) {
    if (offset2.IsEmpty()) {
        return {};
    } else {
        return {ctx.reg_alloc, ctx.reg_alloc};
    }
}

void SwizzleOffsets(EmitContext& ctx, Register off_x, Register off_y, const IR::Value& offset1,
                    const IR::Value& offset2) {
    const Register offsets_a{ctx.reg_alloc.Consume(offset1)};
    const Register offsets_b{ctx.reg_alloc.Consume(offset2)};
    // Input swizzle:  [XYXY] [XYXY]
    // Output swizzle: [XXXX] [YYYY]
    ctx.Add("MOV {}.x,{}.x;"
            "MOV {}.y,{}.z;"
            "MOV {}.z,{}.x;"
            "MOV {}.w,{}.z;"
            "MOV {}.x,{}.y;"
            "MOV {}.y,{}.w;"
            "MOV {}.z,{}.y;"
            "MOV {}.w,{}.w;",
            off_x, offsets_a, off_x, offsets_a, off_x, offsets_b, off_x, offsets_b, off_y,
            offsets_a, off_y, offsets_a, off_y, offsets_b, off_y, offsets_b);
}

std::pair<std::string, ScopedRegister> Coord(EmitContext& ctx, const IR::Value& coord) {
    if (coord.IsImmediate()) {
        ScopedRegister scoped_reg(ctx.reg_alloc);
        return {fmt::to_string(scoped_reg.reg), std::move(scoped_reg)};
    }
    std::string coord_vec{fmt::to_string(Register{ctx.reg_alloc.Consume(coord)})};
    if (coord.InstRecursive()->HasUses()) {
        // Move non-dead coords to a separate register, although this should never happen because
        // vectors are only assembled for immediate texture instructions
        ctx.Add("MOV.F RC,{};", coord_vec);
        coord_vec = "RC";
    }
    return {std::move(coord_vec), ScopedRegister{}};
}

void StoreSparse(EmitContext& ctx, IR::Inst* sparse_inst) {
    if (!sparse_inst) {
        return;
    }
    const Register sparse_ret{ctx.reg_alloc.Define(*sparse_inst)};
    ctx.Add("MOV.S {},-1;"
            "MOV.S {}(NONRESIDENT),0;",
            sparse_ret, sparse_ret);
    sparse_inst->Invalidate();
}
} // Anonymous namespace

void EmitImageSampleImplicitLod(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                                const IR::Value& coord, Register bias_lc, const IR::Value& offset) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto sparse_inst{inst.GetAssociatedPseudoOperation(IR::Opcode::GetSparseFromOp)};
    const std::string_view sparse_mod{sparse_inst ? ".SPARSE" : ""};
    const std::string_view lod_clamp_mod{info.has_lod_clamp ? ".LODCLAMP" : ""};
    const std::string_view type{TextureType(info)};
    const std::string texture{Texture(ctx, info, index)};
    const std::string offset_vec{Offset(ctx, offset)};
    const auto [coord_vec, coord_alloc]{Coord(ctx, coord)};
    const Register ret{ctx.reg_alloc.Define(inst)};
    if (info.has_bias) {
        if (info.type == TextureType::ColorArrayCube) {
            ctx.Add("TXB.F{}{} {},{},{},{},ARRAYCUBE{};", lod_clamp_mod, sparse_mod, ret, coord_vec,
                    bias_lc, texture, offset_vec);
        } else {
            if (info.has_lod_clamp) {
                ctx.Add("MOV.F {}.w,{}.x;"
                        "TXB.F.LODCLAMP{} {},{},{}.y,{},{}{};",
                        coord_vec, bias_lc, sparse_mod, ret, coord_vec, bias_lc, texture, type,
                        offset_vec);
            } else {
                ctx.Add("MOV.F {}.w,{}.x;"
                        "TXB.F{} {},{},{},{}{};",
                        coord_vec, bias_lc, sparse_mod, ret, coord_vec, texture, type, offset_vec);
            }
        }
    } else {
        if (info.has_lod_clamp && info.type == TextureType::ColorArrayCube) {
            ctx.Add("TEX.F.LODCLAMP{} {},{},{},{},ARRAYCUBE{};", sparse_mod, ret, coord_vec,
                    bias_lc, texture, offset_vec);
        } else {
            ctx.Add("TEX.F{}{} {},{},{},{}{};", lod_clamp_mod, sparse_mod, ret, coord_vec, texture,
                    type, offset_vec);
        }
    }
    StoreSparse(ctx, sparse_inst);
}

void EmitImageSampleExplicitLod(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                                const IR::Value& coord, ScalarF32 lod, const IR::Value& offset) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto sparse_inst{inst.GetAssociatedPseudoOperation(IR::Opcode::GetSparseFromOp)};
    const std::string_view sparse_mod{sparse_inst ? ".SPARSE" : ""};
    const std::string_view type{TextureType(info)};
    const std::string texture{Texture(ctx, info, index)};
    const std::string offset_vec{Offset(ctx, offset)};
    const auto [coord_vec, coord_alloc]{Coord(ctx, coord)};
    const Register ret{ctx.reg_alloc.Define(inst)};
    if (info.type == TextureType::ColorArrayCube) {
        ctx.Add("TXL.F{} {},{},{},{},ARRAYCUBE{};", sparse_mod, ret, coord_vec, lod, texture,
                offset_vec);
    } else {
        ctx.Add("MOV.F {}.w,{};"
                "TXL.F{} {},{},{},{}{};",
                coord_vec, lod, sparse_mod, ret, coord_vec, texture, type, offset_vec);
    }
    StoreSparse(ctx, sparse_inst);
}

void EmitImageSampleDrefImplicitLod(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                                    const IR::Value& coord, ScalarF32 dref, Register bias_lc,
                                    const IR::Value& offset) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto sparse_inst{inst.GetAssociatedPseudoOperation(IR::Opcode::GetSparseFromOp)};
    const std::string_view sparse_mod{sparse_inst ? ".SPARSE" : ""};
    const std::string_view type{TextureType(info)};
    const std::string texture{Texture(ctx, info, index)};
    const std::string offset_vec{Offset(ctx, offset)};
    const auto [coord_vec, coord_alloc]{Coord(ctx, coord)};
    const Register ret{ctx.reg_alloc.Define(inst)};
    if (info.has_bias) {
        if (info.has_lod_clamp) {
            switch (info.type) {
            case TextureType::Color1D:
            case TextureType::ColorArray1D:
            case TextureType::Color2D:
                ctx.Add("MOV.F {}.z,{};"
                        "MOV.F {}.w,{}.x;"
                        "TXB.F.LODCLAMP{} {},{},{}.y,{},{}{};",
                        coord_vec, dref, coord_vec, bias_lc, sparse_mod, ret, coord_vec, bias_lc,
                        texture, type, offset_vec);
                break;
            case TextureType::ColorArray2D:
            case TextureType::ColorCube:
                ctx.Add("MOV.F {}.w,{};"
                        "TXB.F.LODCLAMP{} {},{},{},{},{}{};",
                        coord_vec, dref, sparse_mod, ret, coord_vec, bias_lc, texture, type,
                        offset_vec);
                break;
            default:
                throw NotImplementedException("Invalid type {} with bias and lod clamp",
                                              info.type.Value());
            }
        } else {
            switch (info.type) {
            case TextureType::Color1D:
            case TextureType::ColorArray1D:
            case TextureType::Color2D:
                ctx.Add("MOV.F {}.z,{};"
                        "MOV.F {}.w,{}.x;"
                        "TXB.F{} {},{},{},{}{};",
                        coord_vec, dref, coord_vec, bias_lc, sparse_mod, ret, coord_vec, texture,
                        type, offset_vec);
                break;
            case TextureType::ColorArray2D:
            case TextureType::ColorCube:
                ctx.Add("MOV.F {}.w,{};"
                        "TXB.F{} {},{},{},{},{}{};",
                        coord_vec, dref, sparse_mod, ret, coord_vec, bias_lc, texture, type,
                        offset_vec);
                break;
            case TextureType::ColorArrayCube: {
                const ScopedRegister pair{ctx.reg_alloc};
                ctx.Add("MOV.F {}.x,{};"
                        "MOV.F {}.y,{}.x;"
                        "TXB.F{} {},{},{},{},{}{};",
                        pair.reg, dref, pair.reg, bias_lc, sparse_mod, ret, coord_vec, pair.reg,
                        texture, type, offset_vec);
                break;
            }
            default:
                throw NotImplementedException("Invalid type {}", info.type.Value());
            }
        }
    } else {
        if (info.has_lod_clamp) {
            if (info.type != TextureType::ColorArrayCube) {
                const bool w_swizzle{info.type == TextureType::ColorArray2D ||
                                     info.type == TextureType::ColorCube};
                const char dref_swizzle{w_swizzle ? 'w' : 'z'};
                ctx.Add("MOV.F {}.{},{};"
                        "TEX.F.LODCLAMP{} {},{},{},{},{}{};",
                        coord_vec, dref_swizzle, dref, sparse_mod, ret, coord_vec, bias_lc, texture,
                        type, offset_vec);
            } else {
                const ScopedRegister pair{ctx.reg_alloc};
                ctx.Add("MOV.F {}.x,{};"
                        "MOV.F {}.y,{};"
                        "TEX.F.LODCLAMP{} {},{},{},{},{}{};",
                        pair.reg, dref, pair.reg, bias_lc, sparse_mod, ret, coord_vec, pair.reg,
                        texture, type, offset_vec);
            }
        } else {
            if (info.type != TextureType::ColorArrayCube) {
                const bool w_swizzle{info.type == TextureType::ColorArray2D ||
                                     info.type == TextureType::ColorCube};
                const char dref_swizzle{w_swizzle ? 'w' : 'z'};
                ctx.Add("MOV.F {}.{},{};"
                        "TEX.F{} {},{},{},{}{};",
                        coord_vec, dref_swizzle, dref, sparse_mod, ret, coord_vec, texture, type,
                        offset_vec);
            } else {
                const ScopedRegister pair{ctx.reg_alloc};
                ctx.Add("TEX.F{} {},{},{},{},{}{};", sparse_mod, ret, coord_vec, dref, texture,
                        type, offset_vec);
            }
        }
    }
    StoreSparse(ctx, sparse_inst);
}

void EmitImageSampleDrefExplicitLod(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                                    const IR::Value& coord, ScalarF32 dref, ScalarF32 lod,
                                    const IR::Value& offset) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto sparse_inst{inst.GetAssociatedPseudoOperation(IR::Opcode::GetSparseFromOp)};
    const std::string_view sparse_mod{sparse_inst ? ".SPARSE" : ""};
    const std::string_view type{TextureType(info)};
    const std::string texture{Texture(ctx, info, index)};
    const std::string offset_vec{Offset(ctx, offset)};
    const auto [coord_vec, coord_alloc]{Coord(ctx, coord)};
    const Register ret{ctx.reg_alloc.Define(inst)};
    switch (info.type) {
    case TextureType::Color1D:
    case TextureType::ColorArray1D:
    case TextureType::Color2D:
        ctx.Add("MOV.F {}.z,{};"
                "MOV.F {}.w,{};"
                "TXL.F{} {},{},{},{}{};",
                coord_vec, dref, coord_vec, lod, sparse_mod, ret, coord_vec, texture, type,
                offset_vec);
        break;
    case TextureType::ColorArray2D:
    case TextureType::ColorCube:
        ctx.Add("MOV.F {}.w,{};"
                "TXL.F{} {},{},{},{},{}{};",
                coord_vec, dref, sparse_mod, ret, coord_vec, lod, texture, type, offset_vec);
        break;
    case TextureType::ColorArrayCube: {
        const ScopedRegister pair{ctx.reg_alloc};
        ctx.Add("MOV.F {}.x,{};"
                "MOV.F {}.y,{};"
                "TXL.F{} {},{},{},{},{}{};",
                pair.reg, dref, pair.reg, lod, sparse_mod, ret, coord_vec, pair.reg, texture, type,
                offset_vec);
        break;
    }
    default:
        throw NotImplementedException("Invalid type {}", info.type.Value());
    }
    StoreSparse(ctx, sparse_inst);
}

void EmitImageGather(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                     const IR::Value& coord, const IR::Value& offset, const IR::Value& offset2) {
    // Allocate offsets early so they don't overwrite any consumed register
    const auto [off_x, off_y]{AllocOffsetsRegs(ctx, offset2)};
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const char comp{"xyzw"[info.gather_component]};
    const auto sparse_inst{inst.GetAssociatedPseudoOperation(IR::Opcode::GetSparseFromOp)};
    const std::string_view sparse_mod{sparse_inst ? ".SPARSE" : ""};
    const std::string_view type{TextureType(info)};
    const std::string texture{Texture(ctx, info, index)};
    const Register coord_vec{ctx.reg_alloc.Consume(coord)};
    const Register ret{ctx.reg_alloc.Define(inst)};
    if (offset2.IsEmpty()) {
        const std::string offset_vec{Offset(ctx, offset)};
        ctx.Add("TXG.F{} {},{},{}.{},{}{};", sparse_mod, ret, coord_vec, texture, comp, type,
                offset_vec);
    } else {
        SwizzleOffsets(ctx, off_x.reg, off_y.reg, offset, offset2);
        ctx.Add("TXGO.F{} {},{},{},{},{}.{},{};", sparse_mod, ret, coord_vec, off_x.reg, off_y.reg,
                texture, comp, type);
    }
    StoreSparse(ctx, sparse_inst);
}

void EmitImageGatherDref(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                         const IR::Value& coord, const IR::Value& offset, const IR::Value& offset2,
                         const IR::Value& dref) {
    // FIXME: This instruction is not working as expected

    // Allocate offsets early so they don't overwrite any consumed register
    const auto [off_x, off_y]{AllocOffsetsRegs(ctx, offset2)};
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto sparse_inst{inst.GetAssociatedPseudoOperation(IR::Opcode::GetSparseFromOp)};
    const std::string_view sparse_mod{sparse_inst ? ".SPARSE" : ""};
    const std::string_view type{TextureType(info)};
    const std::string texture{Texture(ctx, info, index)};
    const Register coord_vec{ctx.reg_alloc.Consume(coord)};
    const ScalarF32 dref_value{ctx.reg_alloc.Consume(dref)};
    const Register ret{ctx.reg_alloc.Define(inst)};
    std::string args;
    switch (info.type) {
    case TextureType::Color2D:
        ctx.Add("MOV.F {}.z,{};", coord_vec, dref_value);
        args = fmt::to_string(coord_vec);
        break;
    case TextureType::ColorArray2D:
    case TextureType::ColorCube:
        ctx.Add("MOV.F {}.w,{};", coord_vec, dref_value);
        args = fmt::to_string(coord_vec);
        break;
    case TextureType::ColorArrayCube:
        args = fmt::format("{},{}", coord_vec, dref_value);
        break;
    default:
        throw NotImplementedException("Invalid type {}", info.type.Value());
    }
    if (offset2.IsEmpty()) {
        const std::string offset_vec{Offset(ctx, offset)};
        ctx.Add("TXG.F{} {},{},{},{}{};", sparse_mod, ret, args, texture, type, offset_vec);
    } else {
        SwizzleOffsets(ctx, off_x.reg, off_y.reg, offset, offset2);
        ctx.Add("TXGO.F{} {},{},{},{},{},{};", sparse_mod, ret, args, off_x.reg, off_y.reg, texture,
                type);
    }
    StoreSparse(ctx, sparse_inst);
}

void EmitImageFetch(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                    const IR::Value& coord, const IR::Value& offset, ScalarS32 lod, ScalarS32 ms) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const auto sparse_inst{inst.GetAssociatedPseudoOperation(IR::Opcode::GetSparseFromOp)};
    const std::string_view sparse_mod{sparse_inst ? ".SPARSE" : ""};
    const std::string_view type{TextureType(info)};
    const std::string texture{Texture(ctx, info, index)};
    const std::string offset_vec{Offset(ctx, offset)};
    const auto [coord_vec, coord_alloc]{Coord(ctx, coord)};
    const Register ret{ctx.reg_alloc.Define(inst)};
    if (info.type == TextureType::Buffer) {
        ctx.Add("TXF.F{} {},{},{},{}{};", sparse_mod, ret, coord_vec, texture, type, offset_vec);
    } else if (ms.type != Type::Void) {
        ctx.Add("MOV.S {}.w,{};"
                "TXFMS.F{} {},{},{},{}{};",
                coord_vec, ms, sparse_mod, ret, coord_vec, texture, type, offset_vec);
    } else {
        ctx.Add("MOV.S {}.w,{};"
                "TXF.F{} {},{},{},{}{};",
                coord_vec, lod, sparse_mod, ret, coord_vec, texture, type, offset_vec);
    }
    StoreSparse(ctx, sparse_inst);
}

void EmitImageQueryDimensions(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                              ScalarF32 lod) {
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const std::string texture{Texture(ctx, info, index)};
    const std::string_view type{TextureType(info)};
    ctx.Add("TXQ {},{},{},{};", inst, lod, texture, type);
}

void EmitImageQueryLod([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                       [[maybe_unused]] const IR::Value& index, [[maybe_unused]] Register coord) {
    throw NotImplementedException("GLASM instruction");
}

void EmitImageGradient([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                       [[maybe_unused]] const IR::Value& index, [[maybe_unused]] Register coord,
                       [[maybe_unused]] Register derivates, [[maybe_unused]] Register offset,
                       [[maybe_unused]] Register lod_clamp) {
    throw NotImplementedException("GLASM instruction");
}

void EmitImageRead([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                   [[maybe_unused]] const IR::Value& index, [[maybe_unused]] Register coord) {
    throw NotImplementedException("GLASM instruction");
}

void EmitImageWrite([[maybe_unused]] EmitContext& ctx, [[maybe_unused]] IR::Inst& inst,
                    [[maybe_unused]] const IR::Value& index, [[maybe_unused]] Register coord,
                    [[maybe_unused]] Register color) {
    throw NotImplementedException("GLASM instruction");
}

void EmitBindlessImageSampleImplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageSampleExplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageSampleDrefImplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageSampleDrefExplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageGather(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageGatherDref(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageFetch(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageQueryDimensions(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageQueryLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageGradient(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageRead(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBindlessImageWrite(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageSampleImplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageSampleExplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageSampleDrefImplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageSampleDrefExplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageGather(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageGatherDref(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageFetch(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageQueryDimensions(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageQueryLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageGradient(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageRead(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitBoundImageWrite(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

} // namespace Shader::Backend::GLASM
