#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <cstddef>
#include <utility>
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
    Call,
    Return,
};

struct Instruction {
    OpCode op{OpCode::Return};
    std::int64_t operand{0};
};

enum class ValueKind {
    Int64,
    Bool,
    Object,
    Nil,
};

struct SignatureValue {
    ValueKind kind{ValueKind::Nil};
    std::shared_ptr<SignatureValue> left;
    std::shared_ptr<SignatureValue> right;

    [[nodiscard]] bool has_pair_fields() const {
        return kind == ValueKind::Object && left != nullptr && right != nullptr;
    }
};

inline SignatureValue signature_value(ValueKind kind) {
    SignatureValue value;
    value.kind = kind;
    return value;
}

inline SignatureValue pair_signature(SignatureValue left, SignatureValue right) {
    SignatureValue value;
    value.kind = ValueKind::Object;
    value.left = std::make_shared<SignatureValue>(std::move(left));
    value.right = std::make_shared<SignatureValue>(std::move(right));
    return value;
}

struct FunctionSignature {
    std::vector<ValueKind> parameters;
    ValueKind return_type{ValueKind::Int64};
    std::vector<SignatureValue> parameter_types;
    std::optional<SignatureValue> return_type_detail;
};

struct StackMap {
    // Stack maps describe the abstract stack before executing the instruction at the same pc.
    // Bit i is true only when stack slot i is proven to contain an object reference.
    std::vector<bool> object_slots;
};

struct Function {
    FunctionSignature signature;
    std::vector<Instruction> code;
    std::vector<StackMap> stack_maps;
    std::uint32_t local_count{0};
};

struct Module {
    std::vector<Function> functions;
    std::size_t entry_function{0};
};

struct VerificationResult {
    std::vector<StackMap> stack_maps;
};

struct ModuleVerificationResult {
    std::vector<VerificationResult> functions;
};

std::optional<VerificationResult> verify_with_stack_maps(const Function& function);
std::optional<ModuleVerificationResult> verify_with_stack_maps(const Module& module);
bool verify(const Function& function);
bool verify(const Module& module);

} // namespace lang
