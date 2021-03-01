// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
[[nodiscard]] IR::U1 IntegerCompare(TranslatorVisitor& v, const IR::U32& operand_1,
                                    const IR::U32& operand_2, ComparisonOp compare_op,
                                    bool is_signed);

[[nodiscard]] IR::U1 PredicateCombine(TranslatorVisitor& v, const IR::U1& predicate_1,
                                      const IR::U1& predicate_2, BooleanOp bop);
} // namespace Shader::Maxwell