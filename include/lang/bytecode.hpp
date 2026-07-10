#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <cstddef>
#include <string>
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
    AllocArray,
    ArrayGet,
    ArraySet,
    ArrayLen,
    LoadLocal,
    StoreLocal,
    Jump,
    JumpIfFalse,
    Collect,
    Call,
    Return,
    Nil,
    IsNil,
    AllocRefArray,
    RefArrayGet,
    RefArraySet,
    PushStr,
    StrLen,
    StrEq,
    StrConcat,
    StrIndex,
};

struct Instruction {
    OpCode op{OpCode::Return};
    std::int64_t operand{0};
};

enum class ValueKind {
    Int64,
    Bool,
    Object,
    Array,
    Nil,
    Str,
};

struct SignatureValue {
    ValueKind kind{ValueKind::Nil};
    std::shared_ptr<SignatureValue> left;
    std::shared_ptr<SignatureValue> right;
    std::shared_ptr<SignatureValue> element;
    std::optional<std::size_t> named_type;

    [[nodiscard]] bool has_pair_fields() const {
        return kind == ValueKind::Object && !named_type.has_value() &&
               left != nullptr && right != nullptr && element == nullptr;
    }

    [[nodiscard]] bool has_array_element() const {
        return kind == ValueKind::Array && element != nullptr &&
               left == nullptr && right == nullptr && !named_type.has_value();
    }

    [[nodiscard]] bool is_named_type_reference() const {
        return kind == ValueKind::Object && named_type.has_value();
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

inline SignatureValue named_type_signature(std::size_t index) {
    SignatureValue value;
    value.kind = ValueKind::Object;
    value.named_type = index;
    return value;
}

inline SignatureValue array_signature(SignatureValue element) {
    SignatureValue value;
    value.kind = ValueKind::Array;
    value.element = std::make_shared<SignatureValue>(std::move(element));
    return value;
}

struct NamedTypeSignature {
    std::string name;
    SignatureValue body;
};

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
    std::vector<NamedTypeSignature> named_types;
    std::vector<std::string> string_constants;
};

struct VerificationResult {
    std::vector<StackMap> stack_maps;
};

struct ModuleVerificationResult {
    std::vector<VerificationResult> functions;
};

struct VerifiedModuleReport;

class VerifiedModule {
public:
    VerifiedModule(const VerifiedModule&) = default;
    VerifiedModule(VerifiedModule&&) noexcept = default;
    VerifiedModule& operator=(const VerifiedModule&) = default;
    VerifiedModule& operator=(VerifiedModule&&) noexcept = default;

    [[nodiscard]] const Module& module() const { return *module_; }
    [[nodiscard]] const ModuleVerificationResult& verification() const {
        return verification_;
    }

private:
    friend VerifiedModuleReport verify_module_with_diagnostics(Module module);

    VerifiedModule(Module module, ModuleVerificationResult verification)
        : module_(std::make_shared<const Module>(std::move(module))),
          verification_(std::move(verification)) {}

    std::shared_ptr<const Module> module_;
    ModuleVerificationResult verification_;
};

// Stable verifier rejection categories. Keep names append-only when possible:
// callers and fuzz failures use these as machine-readable diagnostics.
enum class VerifierReason {
    ModuleShapeMismatch,       // Module has no functions or an invalid entry function.
    EmptyFunction,             // Function body has no bytecode to enter.
    SignatureShapeMismatch,    // Detailed signature metadata disagrees with coarse kinds.
    LocalCountMismatch,        // Function locals cannot hold declared parameters.
    BadStackMap,               // Supplied stack map count, height, or object bits are wrong.
    StackUnderflow,            // Instruction needs more stack operands than are available.
    TypeMismatch,              // Instruction consumed a value of the wrong kind.
    PoisonUse,                 // Merged incompatible value kind was consumed or mapped.
    UninitializedLocal,        // LoadLocal reads a local not initialized on all paths.
    BadLocalIndex,             // LoadLocal/StoreLocal operand is outside local_count.
    BadJumpTarget,             // Jump/JumpIfFalse target is outside the function body.
    FallOffEnd,                // Non-Return instruction would fall through past code end.
    StackHeightMergeMismatch,  // Control-flow merge has different incoming stack heights.
    UnreachableCode,           // A bytecode pc has no verifier state after analysis.
    BadPairFieldRead,          // Pair field facts are absent, opaque, or poisoned.
    BadPairFieldWrite,         // Pair field write violates available field facts.
    BadArrayOperation,         // Array operation consumes the wrong stack shape or kind.
    BadCallTarget,             // Call operand does not name a function in the module.
    BadCallArity,              // Call stack does not contain all callee arguments.
    BadCallArgKind,            // Call argument kind or detailed pair shape is invalid.
    BadReturnKind,             // Return value kind or detailed pair shape is invalid.
    InvalidOpcode,             // Instruction opcode is not a known OpCode value.
    BadStringConstantIndex,    // PushStr operand is outside the module string pool.
    BadStringOperation,        // String operation consumes the wrong stack shape or kind.
};

struct VerifierDiagnostic {
    std::size_t function_index{0};
    std::optional<std::size_t> pc;
    VerifierReason reason{VerifierReason::InvalidOpcode};
    std::string message;
};

struct FunctionVerifierReport {
    std::optional<VerificationResult> result;
    std::vector<VerifierDiagnostic> diagnostics;
};

struct ModuleVerifierReport {
    std::optional<ModuleVerificationResult> result;
    std::vector<VerifierDiagnostic> diagnostics;
};

struct VerifiedModuleReport {
    std::optional<VerifiedModule> module;
    std::vector<VerifierDiagnostic> diagnostics;
};

const char* verifier_reason_name(VerifierReason reason);
std::string format_verifier_diagnostic(const VerifierDiagnostic& diagnostic);

FunctionVerifierReport verify_with_diagnostics(const Function& function);
ModuleVerifierReport verify_with_diagnostics(const Module& module);
VerifiedModuleReport verify_module_with_diagnostics(Module module);
std::optional<VerificationResult> verify_with_stack_maps(const Function& function);
std::optional<ModuleVerificationResult> verify_with_stack_maps(const Module& module);
std::optional<VerifiedModule> verify_module(Module module);
bool verify(const Function& function);
bool verify(const Module& module);

} // namespace lang
