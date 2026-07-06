#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace lang {

enum class OpCode {
    ConstantI64,
    AddI64,
    LessI64,
    AllocPair,
    GetLeft,
    GetRight,
    SetLeft,
    SetRight,
    LoadLocal,
    StoreLocal,
    Jump,
    JumpIfFalse,
    Collect,
    Return,
};

struct Instruction {
    OpCode op{OpCode::Return};
    std::int64_t operand{0};
};

struct StackMap {
    // Stack maps describe the abstract stack before executing the instruction at the same pc.
    // Bit i is true only when stack slot i is proven to contain an object reference.
    std::vector<bool> object_slots;
};

struct Function {
    std::vector<Instruction> code;
    std::vector<StackMap> stack_maps;
    std::uint32_t local_count{0};
};

struct VerificationResult {
    std::vector<StackMap> stack_maps;
};

std::optional<VerificationResult> verify_with_stack_maps(const Function& function);
bool verify(const Function& function);

} // namespace lang
