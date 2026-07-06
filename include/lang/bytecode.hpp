#pragma once

#include <cstdint>
#include <vector>

namespace lang {

enum class OpCode {
    ConstantI64,
    AddI64,
    AllocPair,
    LoadLocal,
    StoreLocal,
    Collect,
    Return,
};

struct Instruction {
    OpCode op{OpCode::Return};
    std::int64_t operand{0};
};

struct StackMap {
    // For phase 3: bit i says stack slot i is an object reference at this pc.
    std::vector<bool> object_slots;
};

struct Function {
    std::vector<Instruction> code;
    std::vector<StackMap> stack_maps;
    std::uint32_t local_count{0};
};

bool verify(const Function& function);

} // namespace lang
