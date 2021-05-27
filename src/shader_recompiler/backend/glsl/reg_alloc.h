// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <bitset>

#include "common/bit_field.h"
#include "common/common_types.h"

namespace Shader::IR {
class Inst;
class Value;
enum class Type;
} // namespace Shader::IR

namespace Shader::Backend::GLSL {
enum class Type : u32 {
    U1,
    F16x2,
    S32,
    U32,
    F32,
    S64,
    U64,
    F64,
    U32x2,
    F32x2,
    U32x3,
    F32x3,
    U32x4,
    F32x4,
    Void,
};

struct Id {
    union {
        u32 raw;
        BitField<0, 1, u32> is_valid;
        BitField<1, 1, u32> is_long;
        BitField<2, 1, u32> is_spill;
        BitField<3, 1, u32> is_condition_code;
        BitField<4, 1, u32> is_null;
        BitField<5, 27, u32> index;
    };

    bool operator==(Id rhs) const noexcept {
        return raw == rhs.raw;
    }
    bool operator!=(Id rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(sizeof(Id) == sizeof(u32));

class RegAlloc {
public:
    std::string Define(IR::Inst& inst);
    std::string Define(IR::Inst& inst, Type type);
    std::string Define(IR::Inst& inst, IR::Type type);

    std::string Consume(const IR::Value& value);
    std::string GetGlslType(Type type);
    std::string GetGlslType(IR::Type type);

private:
    static constexpr size_t NUM_REGS = 4096;
    static constexpr size_t NUM_ELEMENTS = 4;

    std::string Consume(IR::Inst& inst);
    Type RegType(IR::Type type);
    Id Alloc();
    void Free(Id id);

    size_t num_used_registers{};
    std::bitset<NUM_REGS> register_use{};
    std::bitset<NUM_REGS> register_defined{};
};

} // namespace Shader::Backend::GLSL
