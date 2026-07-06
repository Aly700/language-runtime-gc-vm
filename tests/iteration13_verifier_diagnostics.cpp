#include "lang/bytecode.hpp"
#include "lang/vm.hpp"

#include <exception>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void set_signature(lang::Function& function,
                   std::initializer_list<lang::ValueKind> parameters,
                   lang::ValueKind result) {
    function.signature.parameters.assign(parameters.begin(), parameters.end());
    function.signature.return_type = result;
}

lang::SignatureValue sig_i64() {
    return lang::signature_value(lang::ValueKind::Int64);
}

lang::SignatureValue sig_bool() {
    return lang::signature_value(lang::ValueKind::Bool);
}

lang::SignatureValue sig_pair(lang::SignatureValue left, lang::SignatureValue right) {
    return lang::pair_signature(std::move(left), std::move(right));
}

void set_typed_signature(lang::Function& function,
                         std::vector<lang::SignatureValue> parameters,
                         lang::SignatureValue result) {
    function.signature.parameter_types = std::move(parameters);
    function.signature.parameters.clear();
    function.signature.parameters.reserve(function.signature.parameter_types.size());
    for (const auto& parameter : function.signature.parameter_types) {
        function.signature.parameters.push_back(parameter.kind);
    }
    function.signature.return_type = result.kind;
    function.signature.return_type_detail = std::move(result);
}

std::string diag_context(const lang::VerifierDiagnostic& diagnostic) {
    std::ostringstream out;
    out << "function=" << diagnostic.function_index;
    if (diagnostic.pc.has_value()) {
        out << " pc=" << *diagnostic.pc;
    } else {
        out << " pc=<none>";
    }
    out << " reason=" << lang::verifier_reason_name(diagnostic.reason)
        << " message=" << diagnostic.message;
    return out.str();
}

void require_first_diagnostic(const lang::Module& module,
                              lang::VerifierReason expected_reason,
                              std::size_t expected_function,
                              std::optional<std::size_t> expected_pc) {
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value(), "diagnostic report accepted invalid module");
    require(!report.diagnostics.empty(), "rejected module had no verifier diagnostics");

    const auto& diagnostic = report.diagnostics.front();
    const auto context = diag_context(diagnostic);
    require(diagnostic.reason == expected_reason,
            "reason mismatch: " + context);
    require(diagnostic.function_index == expected_function,
            "function index mismatch: " + context);
    require(diagnostic.pc == expected_pc, "pc mismatch: " + context);
    require(!diagnostic.message.empty(), "diagnostic message was empty: " + context);

    require(!lang::verify(module),
            "legacy bool verifier accepted module rejected by diagnostic verifier");
    require(!lang::verify_with_stack_maps(module).has_value(),
            "legacy optional verifier accepted module rejected by diagnostic verifier");
}

void require_first_diagnostic(const lang::Function& function,
                              lang::VerifierReason expected_reason,
                              std::optional<std::size_t> expected_pc) {
    const auto report = lang::verify_with_diagnostics(function);
    require(!report.result.has_value(), "diagnostic report accepted invalid function");
    require(!report.diagnostics.empty(), "rejected function had no verifier diagnostics");

    const auto& diagnostic = report.diagnostics.front();
    const auto context = diag_context(diagnostic);
    require(diagnostic.reason == expected_reason,
            "reason mismatch: " + context);
    require(diagnostic.function_index == 0, "function index mismatch: " + context);
    require(diagnostic.pc == expected_pc, "pc mismatch: " + context);
    require(!diagnostic.message.empty(), "diagnostic message was empty: " + context);

    require(!lang::verify(function),
            "legacy bool verifier accepted function rejected by diagnostic verifier");
    require(!lang::verify_with_stack_maps(function).has_value(),
            "legacy optional verifier accepted function rejected by diagnostic verifier");
}

lang::Function valid_i64_function() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Int64);
    function.code = {
        {lang::OpCode::ConstantI64, 40},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };
    return function;
}

void reports_module_shape_mismatch() {
    lang::Module module;
    require_first_diagnostic(module, lang::VerifierReason::ModuleShapeMismatch, 0,
                             std::nullopt);
}

void reports_empty_function() {
    lang::Function function;
    require_first_diagnostic(function, lang::VerifierReason::EmptyFunction,
                             std::nullopt);
}

void reports_signature_shape_mismatch() {
    lang::Function function = valid_i64_function();
    function.signature.parameters = {lang::ValueKind::Int64};
    function.signature.parameter_types = {sig_bool()};
    function.local_count = 1;

    require_first_diagnostic(function, lang::VerifierReason::SignatureShapeMismatch,
                             std::nullopt);
}

void reports_local_count_mismatch() {
    lang::Function function = valid_i64_function();
    function.signature.parameters = {lang::ValueKind::Int64};
    function.local_count = 0;

    require_first_diagnostic(function, lang::VerifierReason::LocalCountMismatch,
                             std::nullopt);
}

void reports_bad_stack_map() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Object);
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Return, 0},
    };
    function.stack_maps = {
        {{}},
        {{false}},
        {{false, false}},
        {{false}},
    };

    require_first_diagnostic(function, lang::VerifierReason::BadStackMap, 3);
}

void reports_stack_underflow() {
    lang::Function function;
    function.code = {{lang::OpCode::AddI64, 0}};

    require_first_diagnostic(function, lang::VerifierReason::StackUnderflow, 0);
}

void reports_type_mismatch() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::ConstantI64, 3},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(function, lang::VerifierReason::TypeMismatch, 4);
}

void reports_poison_use() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 7},
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::Jump, 10},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(function, lang::VerifierReason::PoisonUse, 11);
}

void reports_uninitialized_local() {
    lang::Function function;
    function.local_count = 1;
    function.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(function, lang::VerifierReason::UninitializedLocal, 0);
}

void reports_bad_local_index() {
    lang::Function function;
    function.local_count = 1;
    function.code = {
        {lang::OpCode::LoadLocal, 3},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(function, lang::VerifierReason::BadLocalIndex, 0);
}

void reports_bad_jump_target() {
    lang::Function function;
    function.code = {{lang::OpCode::Jump, 99}};

    require_first_diagnostic(function, lang::VerifierReason::BadJumpTarget, 0);
}

void reports_fall_off_end() {
    lang::Function function;
    function.code = {{lang::OpCode::ConstantI64, 1}};

    require_first_diagnostic(function, lang::VerifierReason::FallOffEnd, 0);
}

void reports_stack_height_merge_mismatch() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 6},
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::Jump, 6},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(function, lang::VerifierReason::StackHeightMergeMismatch, 6);
}

void reports_unreachable_code() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::Return, 0},
        {lang::OpCode::ConstantI64, 2},
    };

    require_first_diagnostic(function, lang::VerifierReason::UnreachableCode, 2);
}

void reports_bad_pair_field_read() {
    lang::Function function;
    function.local_count = 1;
    set_signature(function, {lang::ValueKind::Object}, lang::ValueKind::Int64);
    function.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::GetLeft, 0},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(function, lang::VerifierReason::BadPairFieldRead, 1);
}

void reports_bad_pair_field_write() {
    lang::Function function;
    function.local_count = 1;
    set_typed_signature(function, {sig_pair(sig_bool(), sig_i64())}, sig_i64());
    function.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::SetLeft, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(function, lang::VerifierReason::BadPairFieldWrite, 2);
}

void reports_bad_call_target() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);
    set_signature(module.functions[0], {}, lang::ValueKind::Int64);
    module.functions[0].code = {
        {lang::OpCode::Call, 7},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(module, lang::VerifierReason::BadCallTarget, 0, 0);
}

void reports_bad_call_arity() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    set_signature(module.functions[0], {}, lang::ValueKind::Int64);
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };
    module.functions[1].local_count = 2;
    set_signature(module.functions[1],
                  {lang::ValueKind::Int64, lang::ValueKind::Int64},
                  lang::ValueKind::Int64);
    module.functions[1].code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(module, lang::VerifierReason::BadCallArity, 0, 1);
}

void reports_bad_call_arg_kind() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    set_signature(module.functions[0], {}, lang::ValueKind::Object);
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };
    module.functions[1].local_count = 1;
    set_signature(module.functions[1], {lang::ValueKind::Object},
                  lang::ValueKind::Object);
    module.functions[1].code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(module, lang::VerifierReason::BadCallArgKind, 0, 1);
}

void reports_bad_return_kind() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Bool);
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::Return, 0},
    };

    require_first_diagnostic(function, lang::VerifierReason::BadReturnKind, 1);
}

void reports_invalid_opcode() {
    lang::Function function;
    function.code = {{static_cast<lang::OpCode>(999), 0}};

    require_first_diagnostic(function, lang::VerifierReason::InvalidOpcode, 0);
}

void new_and_legacy_apis_agree_on_acceptance() {
    const auto valid = valid_i64_function();
    const auto valid_function_report = lang::verify_with_diagnostics(valid);
    require(valid_function_report.result.has_value(),
            "diagnostic function API rejected valid function");
    require(valid_function_report.diagnostics.empty(),
            "diagnostic function API reported diagnostics for valid function");
    require(lang::verify_with_stack_maps(valid).has_value(),
            "legacy function API rejected valid function");

    lang::Module valid_module;
    valid_module.entry_function = 0;
    valid_module.functions.push_back(valid);
    const auto valid_module_report = lang::verify_with_diagnostics(valid_module);
    require(valid_module_report.result.has_value(),
            "diagnostic module API rejected valid module");
    require(valid_module_report.diagnostics.empty(),
            "diagnostic module API reported diagnostics for valid module");
    require(lang::verify_with_stack_maps(valid_module).has_value(),
            "legacy module API rejected valid module");

    lang::Function invalid;
    invalid.code = {{lang::OpCode::AddI64, 0}};
    const auto invalid_report = lang::verify_with_diagnostics(invalid);
    require(invalid_report.result.has_value() ==
                lang::verify_with_stack_maps(invalid).has_value(),
            "diagnostic and legacy function APIs disagreed on invalid acceptance");
}

void vm_exception_includes_first_diagnostic() {
    lang::Function function;
    function.code = {{lang::OpCode::AddI64, 0}};
    lang::VM vm;
    try {
        (void)vm.execute(function);
    } catch (const std::exception& e) {
        const std::string message = e.what();
        require(message.find("StackUnderflow") != std::string::npos,
                "VM verifier exception did not include reason: " + message);
        require(message.find("function=0") != std::string::npos,
                "VM verifier exception did not include function index: " + message);
        require(message.find("pc=0") != std::string::npos,
                "VM verifier exception did not include pc: " + message);
        return;
    }
    throw std::runtime_error("VM accepted invalid function");
}

struct TestCase {
    const char* name;
    const char* proves;
    const char* baseline_red;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"reports_module_shape_mismatch",
         "module-level rejections include a stable reason and no pc",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_module_shape_mismatch},
        {"reports_empty_function",
         "empty functions are rejected with function index and no pc",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_empty_function},
        {"reports_signature_shape_mismatch",
         "malformed detailed signatures are diagnosable",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_signature_shape_mismatch},
        {"reports_local_count_mismatch",
         "parameter/local-count shape failures are diagnosable",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_local_count_mismatch},
        {"reports_bad_stack_map",
         "hand-written stack-map mismatches report the mismatching pc",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_bad_stack_map},
        {"reports_stack_underflow",
         "operand stack underflow reports the failing pc",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_stack_underflow},
        {"reports_type_mismatch",
         "wrong operand kinds report TypeMismatch at the consuming pc",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_type_mismatch},
        {"reports_poison_use",
         "joined incompatible kinds report PoisonUse when consumed",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_poison_use},
        {"reports_uninitialized_local",
         "path-uninitialized locals report UninitializedLocal at LoadLocal",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_uninitialized_local},
        {"reports_bad_local_index",
         "out-of-range locals report BadLocalIndex at the access",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_bad_local_index},
        {"reports_bad_jump_target",
         "out-of-range branches report BadJumpTarget at the branch",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_bad_jump_target},
        {"reports_fall_off_end",
         "fallthrough past code end reports FallOffEnd at the instruction",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_fall_off_end},
        {"reports_stack_height_merge_mismatch",
         "different stack heights at a merge report the target pc",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_stack_height_merge_mismatch},
        {"reports_unreachable_code",
         "unvisited bytecode reports UnreachableCode at the first missing pc",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_unreachable_code},
        {"reports_bad_pair_field_read",
         "opaque pair field reads report BadPairFieldRead",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_bad_pair_field_read},
        {"reports_bad_pair_field_write",
         "typed pair field writes report BadPairFieldWrite",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_bad_pair_field_write},
        {"reports_bad_call_target",
         "out-of-range calls report BadCallTarget at the call",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_bad_call_target},
        {"reports_bad_call_arity",
         "too-few call arguments report BadCallArity at the call",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_bad_call_arity},
        {"reports_bad_call_arg_kind",
         "wrong call argument kinds report BadCallArgKind at the call",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_bad_call_arg_kind},
        {"reports_bad_return_kind",
         "wrong return kinds report BadReturnKind at Return",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_bad_return_kind},
        {"reports_invalid_opcode",
         "unknown enum values report InvalidOpcode at the instruction",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         reports_invalid_opcode},
        {"new_and_legacy_apis_agree_on_acceptance",
         "diagnostic and legacy verifier APIs preserve accept/reject behavior",
         "BASELINE-RED on 88eccca: diagnostics API does not exist",
         new_and_legacy_apis_agree_on_acceptance},
        {"vm_exception_includes_first_diagnostic",
         "VM verifier rejection exceptions expose first diagnostic context",
         "BASELINE-RED on 88eccca: VM exception is generic",
         vm_exception_includes_first_diagnostic},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << " | proves: " << test.proves
                      << " | baseline: " << test.baseline_red << "\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << " | proves: " << test.proves
                      << " | baseline: " << test.baseline_red << "\n"
                      << e.what() << "\n";
        }
    }

    if (failures != 0) {
        std::cerr << failures << " iteration-13 verifier diagnostic test(s) failed\n";
        return 1;
    }
    return 0;
}
