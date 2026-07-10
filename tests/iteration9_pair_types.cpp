#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/value.hpp"
#include "lang/vm.hpp"

#include <exception>
#include <iostream>
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

std::string source_listing(const std::string& source) {
    std::ostringstream out;
    out << "source:\n" << source << "\n";
    return out.str();
}

const char* op_name(lang::OpCode op) {
    switch (op) {
    case lang::OpCode::ConstantI64:
        return "ConstantI64";
    case lang::OpCode::AddI64:
        return "AddI64";
    case lang::OpCode::LessI64:
        return "LessI64";
    case lang::OpCode::AllocPair:
        return "AllocPair";
    case lang::OpCode::GetLeft:
        return "GetLeft";
    case lang::OpCode::GetRight:
        return "GetRight";
    case lang::OpCode::SetLeft:
        return "SetLeft";
    case lang::OpCode::SetRight:
        return "SetRight";
    case lang::OpCode::AllocArray:
        return "AllocArray";
    case lang::OpCode::ArrayGet:
        return "ArrayGet";
    case lang::OpCode::ArraySet:
        return "ArraySet";
    case lang::OpCode::ArrayLen:
        return "ArrayLen";
    case lang::OpCode::LoadLocal:
        return "LoadLocal";
    case lang::OpCode::StoreLocal:
        return "StoreLocal";
    case lang::OpCode::Jump:
        return "Jump";
    case lang::OpCode::JumpIfFalse:
        return "JumpIfFalse";
    case lang::OpCode::Collect:
        return "Collect";
    case lang::OpCode::Call:
        return "Call";
    case lang::OpCode::Return:
        return "Return";
    case lang::OpCode::Nil:
        return "Nil";
    case lang::OpCode::IsNil:
        return "IsNil";
    }
    return "<unknown>";
}

std::string describe_module(const lang::Module& module) {
    std::ostringstream out;
    out << "entry=" << module.entry_function << " functions=" << module.functions.size()
        << "\n";
    for (std::size_t function_index = 0; function_index < module.functions.size();
         ++function_index) {
        const auto& function = module.functions[function_index];
        out << "function=" << function_index << " locals=" << function.local_count
            << "\n";
        for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
            out << "  #" << pc << " " << op_name(function.code[pc].op) << " "
                << function.code[pc].operand << "\n";
        }
    }
    return out.str();
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

void set_signature(lang::Function& function, std::vector<lang::SignatureValue> parameters,
                   lang::SignatureValue result) {
    function.signature.parameters.clear();
    function.signature.parameter_types = std::move(parameters);
    function.signature.parameters.reserve(function.signature.parameter_types.size());
    for (const auto& parameter : function.signature.parameter_types) {
        function.signature.parameters.push_back(parameter.kind);
    }
    function.signature.return_type = result.kind;
    function.signature.return_type_detail = std::move(result);
}

struct Schedule {
    const char* name;
    lang::gc::StressConfig stress;
};

std::vector<Schedule> stress_schedules() {
    std::vector<Schedule> schedules;
    schedules.push_back({"no_stress", {}});

    lang::gc::StressConfig every_instruction;
    every_instruction.collect_every_n_instructions = 1;
    every_instruction.collect_minor_every_n_instructions = 1;
    schedules.push_back({"major_and_minor_every_instruction", every_instruction});

    lang::gc::StressConfig after_barrier;
    after_barrier.collect_before_every_allocation = true;
    after_barrier.collect_minor_after_every_write_barrier = true;
    schedules.push_back({"minor_after_every_barrier", after_barrier});
    return schedules;
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::ostringstream out;
        out << "expected source to compile\n" << source_listing(source);
        for (const auto& diagnostic : compiled.diagnostics) {
            out << diagnostic.position.line << ":" << diagnostic.position.column << " "
                << diagnostic.message << "\n";
        }
        throw std::runtime_error(out.str());
    }
    require(compiled.verified_module.has_value(),
            "successful compile did not return a verified module");
    return compiled;
}

lang::Value execute_source(const std::string& source, const Schedule& schedule,
                           lang::VM& vm) {
    const auto compiled = require_compiles(source);
    vm.set_gc_stress(schedule.stress);
    return vm.execute(*compiled.verified_module);
}

void require_i64_result_under_stress(const std::string& source, std::int64_t expected) {
    for (const auto& schedule : stress_schedules()) {
        lang::VM vm;
        const auto result = execute_source(source, schedule, vm);
        require(result.as_i64() == expected,
                std::string("wrong i64 result under ") + schedule.name + "\n" +
                    source_listing(source));
        vm.heap().TEST_ONLY_validate_gc_invariants();
    }
}

void require_bool_result_under_stress(const std::string& source, bool expected) {
    for (const auto& schedule : stress_schedules()) {
        lang::VM vm;
        const auto result = execute_source(source, schedule, vm);
        require(result.as_bool() == expected,
                std::string("wrong bool result under ") + schedule.name + "\n" +
                    source_listing(source));
        vm.heap().TEST_ONLY_validate_gc_invariants();
    }
}

void require_diagnostic(const std::string& source, std::size_t line, std::size_t column,
                        const std::string& expected_message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected source to be rejected\n" + source_listing(source));
    require(!compiled.diagnostics.empty(),
            "rejected source did not include diagnostics\n" + source_listing(source));

    const auto& diagnostic = compiled.diagnostics.front();
    std::ostringstream context;
    context << source_listing(source);
    context << "first diagnostic: " << diagnostic.position.line << ":"
            << diagnostic.position.column << " " << diagnostic.message << "\n";

    require(diagnostic.position.line == line, "diagnostic line mismatch\n" + context.str());
    require(diagnostic.position.column == column,
            "diagnostic column mismatch\n" + context.str());
    require(diagnostic.message.find(expected_message) != std::string::npos,
            "diagnostic message mismatch\n" + context.str());
}

void verifier_accepts_field_read_from_typed_pair_param() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);
    auto& function = module.functions[0];
    function.local_count = 1;
    set_signature(function, {sig_pair(sig_i64(), sig_bool())}, sig_i64());
    function.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::GetLeft, 0},
        {lang::OpCode::Return, 0},
    };

    require(lang::verify(module),
            "verifier rejected typed pair param field read\n" + describe_module(module));
}

void verifier_accepts_field_read_from_typed_pair_return() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    set_signature(module.functions[0], {}, sig_bool());
    module.functions[0].code = {
        {lang::OpCode::Call, 1},
        {lang::OpCode::GetRight, 0},
        {lang::OpCode::Return, 0},
    };

    auto& make = module.functions[1];
    set_signature(make, {}, sig_pair(sig_i64(), sig_bool()));
    make.code = {
        {lang::OpCode::ConstantI64, 7},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Return, 0},
    };

    require(lang::verify(module),
            "verifier rejected typed pair return field read\n" + describe_module(module));
}

void verifier_rejects_call_argument_with_wrong_declared_pair_fields() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    set_signature(module.functions[0], {}, sig_i64());
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };

    auto& take = module.functions[1];
    take.local_count = 1;
    set_signature(take, {sig_pair(sig_i64(), sig_bool())}, sig_i64());
    take.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(module),
            "verifier accepted call argument with wrong declared pair fields\n" +
                describe_module(module));
}

void verifier_rejects_return_with_wrong_declared_pair_fields() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);
    set_signature(module.functions[0], {}, sig_pair(sig_i64(), sig_bool()));
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(module),
            "verifier accepted return with wrong declared pair fields\n" +
                describe_module(module));
}

void typed_pair_param_fields_execute_under_gc_stress() {
    const std::string source = R"SRC(
fn bump(p: pair<i64, pair>) -> i64 {
  p.left = p.left + 1;
  p.left
}
let seed: pair = pair(0, 0);
let p: pair<i64, pair> = pair(41, seed);
bump(p)
)SRC";

    require_i64_result_under_stress(source, 42);
}

void typed_pair_return_fields_execute_under_gc_stress() {
    const std::string source = R"SRC(
fn make(x: i64) -> pair<i64, bool> {
  pair(x + 1, true)
}
make(41).left
)SRC";

    require_i64_result_under_stress(source, 42);
}

void nested_typed_pair_fields_execute_across_call_boundary() {
    const std::string source = R"SRC(
fn choose(p: pair<pair<i64, bool>, i64>) -> bool {
  p.left.right
}
let inner: pair<i64, bool> = pair(7, true);
choose(pair(inner, 9))
)SRC";

    require_bool_result_under_stress(source, true);
}

void rejects_typed_pair_construction_mismatch_with_position() {
    require_diagnostic("let p: pair<i64, bool> = pair(1, 2);\np.left", 1, 24,
                       "cannot initialize local 'p' of type pair<i64, bool> with pair<i64, i64>");
}

void rejects_typed_pair_field_assignment_mismatch_with_position() {
    const std::string source = R"SRC(
let seed: pair = pair(0, 0);
let p: pair<i64, pair> = pair(1, seed);
p.left = true;
p.left
)SRC";
    require_diagnostic(source, 4, 8,
                       "cannot assign bool to field 'left' of type i64");
}

void rejects_cross_call_pair_field_mismatch_with_position() {
    const std::string source = R"SRC(
fn take(p: pair<i64, bool>) -> i64 {
  p.left
}
let p: pair<i64, i64> = pair(1, 2);
take(p)
)SRC";
    require_diagnostic(source, 6, 6,
                       "argument 1 of function 'take' expects pair<i64, bool> but got pair<i64, i64>");
}

void bare_pair_param_field_reads_remain_rejected() {
    const std::string source = R"SRC(
fn bad(p: pair) -> i64 {
  p.left
}
let p: pair = pair(1, 2);
bad(p)
)SRC";
    require_diagnostic(source, 3, 5, "pair field type is unknown");
}

void cyclic_structure_remains_constructible_with_opaque_pair_leaf() {
    const std::string source = R"SRC(
fn link(a: pair<pair, pair>) -> pair<pair, pair> {
  a.left = a;
  a
}
let seed: pair = pair(0, 0);
let a: pair<pair, pair> = pair(seed, seed);
link(a)
)SRC";

    for (const auto& schedule : stress_schedules()) {
        lang::VM vm;
        const auto result = execute_source(source, schedule, vm);
        require(result.is_object(),
                std::string("cycle program returned non-object under ") + schedule.name);
        require(vm.heap().left(result.as_object()).as_object() == result.as_object(),
                std::string("cycle field did not point back to the result under ") +
                    schedule.name);
        vm.heap().TEST_ONLY_validate_gc_invariants();
    }
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
        {"verifier_accepts_field_read_from_typed_pair_param",
         "typed pair parameter locals enter the verifier with declared field kinds",
         "RED on HEAD 331e8f8: parameter objects have no allocation site and no field facts",
         verifier_accepts_field_read_from_typed_pair_param},
        {"verifier_accepts_field_read_from_typed_pair_return",
         "typed pair call results carry declared return field kinds",
         "RED on HEAD 331e8f8: Call pushes only coarse object kind",
         verifier_accepts_field_read_from_typed_pair_return},
        {"verifier_rejects_call_argument_with_wrong_declared_pair_fields",
         "call arguments must satisfy declared pair<T, U> field kinds",
         "RED after adding signature details: old verifier accepts any object argument",
         verifier_rejects_call_argument_with_wrong_declared_pair_fields},
        {"verifier_rejects_return_with_wrong_declared_pair_fields",
         "Return must satisfy declared pair<T, U> field kinds",
         "RED after adding signature details: old verifier accepts any object return",
         verifier_rejects_return_with_wrong_declared_pair_fields},
        {"typed_pair_param_fields_execute_under_gc_stress",
         "pair<T, U> parameters expose declared field types across call boundaries",
         "RED on HEAD 331e8f8: parser/type checker only accepts monomorphic pair",
         typed_pair_param_fields_execute_under_gc_stress},
        {"typed_pair_return_fields_execute_under_gc_stress",
         "pair<T, U> returns expose declared field types to callers",
         "RED on HEAD 331e8f8: call results carry only object kind",
         typed_pair_return_fields_execute_under_gc_stress},
        {"nested_typed_pair_fields_execute_across_call_boundary",
         "nested pair<T, U> signatures preserve recursive field-kind facts",
         "RED on HEAD 331e8f8: pair field type is unknown after function boundary",
         nested_typed_pair_fields_execute_across_call_boundary},
        {"rejects_typed_pair_construction_mismatch_with_position",
         "typed pair construction must match declared field types and report source position",
         "RED on HEAD 331e8f8: pair<T, U> syntax is rejected before type checking",
         rejects_typed_pair_construction_mismatch_with_position},
        {"rejects_typed_pair_field_assignment_mismatch_with_position",
         "field assignment through typed pairs preserves declared field types",
         "RED on HEAD 331e8f8: pair<T, U> syntax is rejected before assignment checking",
         rejects_typed_pair_field_assignment_mismatch_with_position},
        {"rejects_cross_call_pair_field_mismatch_with_position",
         "call arguments must satisfy full pair<T, U> parameter signatures",
         "RED on HEAD 331e8f8: function signatures do not carry pair field types",
         rejects_cross_call_pair_field_mismatch_with_position},
        {"bare_pair_param_field_reads_remain_rejected",
         "opaque pair parameters still reject field reads with the old diagnostic",
         "Already green on HEAD 331e8f8: documents preserved opaque-pair behavior",
         bare_pair_param_field_reads_remain_rejected},
        {"cyclic_structure_remains_constructible_with_opaque_pair_leaf",
         "bare pair remains an opaque leaf so cyclic structures can still be built by assignment",
         "RED on HEAD 331e8f8: pair<T, U> syntax is rejected",
         cyclic_structure_remains_constructible_with_opaque_pair_leaf},
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
        std::cerr << failures << " iteration-9 pair type test(s) failed\n";
        return 1;
    }
    return 0;
}
