#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/value.hpp"
#include "lang/vm.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
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

std::string value_token(const lang::gc::Heap& heap, lang::Value value,
                        std::map<lang::ObjectId, std::size_t>& indexes,
                        std::vector<lang::ObjectId>& order) {
    std::ostringstream out;
    switch (value.tag()) {
    case lang::Value::Tag::Int64:
        out << "i64:" << value.as_i64();
        return out.str();
    case lang::Value::Tag::Bool:
        out << "bool:" << (value.as_bool() ? "true" : "false");
        return out.str();
    case lang::Value::Tag::Nil:
        return "nil";
    case lang::Value::Tag::Object: {
        const auto id = value.as_object();
        auto it = indexes.find(id);
        if (it == indexes.end()) {
            const auto index = order.size();
            indexes.emplace(id, index);
            order.push_back(id);
            it = indexes.find(id);
        }
        out << "@" << it->second;
        (void)heap.object(id);
        return out.str();
    }
    }
    return "<unknown>";
}

std::string observable(lang::VM& vm, lang::Value value) {
    vm.heap().TEST_ONLY_validate_gc_invariants();

    if (!value.is_object()) {
        std::map<lang::ObjectId, std::size_t> unused_indexes;
        std::vector<lang::ObjectId> unused_order;
        return value_token(vm.heap(), value, unused_indexes, unused_order);
    }

    std::map<lang::ObjectId, std::size_t> indexes;
    std::vector<lang::ObjectId> order;
    std::ostringstream out;
    out << value_token(vm.heap(), value, indexes, order);
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& object = vm.heap().object(order[i]);
        out << "\n@" << i << " = pair("
            << value_token(vm.heap(), object.left, indexes, order) << ", "
            << value_token(vm.heap(), object.right, indexes, order) << ")";
    }
    return out.str();
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
    require(compiled.function.has_value(),
            "successful compile did not return bytecode\n" + source_listing(source));
    return compiled;
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

lang::Value execute_source(const std::string& source, lang::VM& vm) {
    auto compiled = require_compiles(source);
    return vm.execute(*compiled.function);
}

void rejects_undefined_variable_with_position() {
    require_diagnostic("missing", 1, 1, "undefined variable 'missing'");
}

void rejects_plus_operand_type_mismatch_with_position() {
    require_diagnostic("true + 1", 1, 6, "operator '+' requires i64 operands");
}

void rejects_less_operand_type_mismatch_with_position() {
    require_diagnostic("1 < false", 1, 3, "operator '<' requires i64 operands");
}

void rejects_if_condition_type_mismatch_with_position() {
    require_diagnostic("if 1 { } else { } 0", 1, 4, "if condition must be bool");
}

void rejects_assignment_type_mismatch_with_position() {
    require_diagnostic("let x: i64 = 1; x = true; x", 1, 19,
                       "cannot assign bool to local 'x' of type i64");
}

void rejects_field_access_on_non_pair_with_position() {
    require_diagnostic("let x: i64 = 1; x.left", 1, 19, "field access requires pair");
}

void rejects_field_assignment_type_mismatch_with_position() {
    require_diagnostic("let p: pair = pair(1, 2); p.left = true; p", 1, 34,
                       "cannot assign bool to field 'left' of type i64");
}

void executes_scalar_loop_end_to_end() {
    const std::string source = R"SRC(
let i: i64 = 0;
let acc: i64 = 0;
while i < 5 {
  acc = acc + i;
  i = i + 1;
}
acc
)SRC";

    lang::VM vm;
    const auto result = execute_source(source, vm);
    require(result.as_i64() == 10,
            "scalar loop returned wrong result\n" + source_listing(source));
}

void executes_if_else_end_to_end() {
    const std::string source = R"SRC(
let x: i64 = 1;
if x < 2 {
  x = 7;
} else {
  x = 9;
}
x
)SRC";

    lang::VM vm;
    const auto result = execute_source(source, vm);
    require(result.as_i64() == 7,
            "if/else assignment returned wrong result\n" + source_listing(source));
}

void executes_pair_field_access_and_mutation_end_to_end() {
    const std::string source = R"SRC(
let p: pair = pair(1, 2);
p.left = p.left + 41;
p.left
)SRC";

    lang::VM vm;
    const auto result = execute_source(source, vm);
    require(result.as_i64() == 42,
            "pair field mutation returned wrong result\n" + source_listing(source));
}

void executes_loop_building_linked_structure_end_to_end() {
    const std::string source = R"SRC(
let tail: pair = pair(99, 100);
let head: pair = tail;
let i: i64 = 0;
while i < 4 {
  head = pair(i, head);
  i = i + 1;
}
head.left
)SRC";

    lang::VM vm;
    const auto result = execute_source(source, vm);
    require(result.as_i64() == 3,
            "linked-structure loop returned wrong head value\n" + source_listing(source));
}

void executes_pair_cycle_created_by_field_assignment() {
    const std::string source = R"SRC(
let seed: pair = pair(0, 0);
let a: pair = pair(seed, seed);
let b: pair = pair(seed, seed);
a.left = b;
b.right = a;
a
)SRC";

    lang::VM vm;
    const auto result = execute_source(source, vm);
    require(result.is_object(), "cycle program returned non-pair\n" + source_listing(source));
    const auto a = result.as_object();
    const auto b = vm.heap().left(a).as_object();
    require(vm.heap().right(b).as_object() == a,
            "cycle through a.left.right did not point back to a\n" + source_listing(source));
}

std::vector<std::string> agreement_corpus() {
    return {
        "1 + 2",
        "true",
        "1 < 2",
        "let p: pair = pair(1, 2); p.right",
        "let p: pair = pair(true, 4); if p.left { p.right = p.right + 1; } else { p.right = 0; } p.right",
        R"SRC(
let i: i64 = 0;
let acc: i64 = 0;
while i < 6 {
  acc = acc + i;
  i = i + 1;
}
acc
)SRC",
        R"SRC(
let tail: pair = pair(0, 0);
let head: pair = tail;
let i: i64 = 0;
while i < 3 {
  head = pair(i, head);
  i = i + 1;
}
head.left
)SRC",
        R"SRC(
let seed: pair = pair(0, 0);
let a: pair = pair(seed, seed);
let b: pair = pair(seed, seed);
a.left = b;
b.right = a;
a
)SRC",
    };
}

void compiled_type_checked_programs_pass_verifier_with_stack_maps() {
    for (const auto& source : agreement_corpus()) {
        const auto compiled = require_compiles(source);
        require(lang::verify_with_stack_maps(*compiled.function).has_value(),
                "compiled function failed verifier agreement test\n" + source_listing(source));
        require(compiled.function->stack_maps.size() == compiled.function->code.size(),
                "compiler did not attach verifier-generated stack maps\n" +
                    source_listing(source));
    }
}

struct Schedule {
    const char* name;
    lang::gc::StressConfig stress;
};

std::vector<Schedule> stress_schedules() {
    std::vector<Schedule> schedules;
    schedules.push_back({"no_stress", {}});

    lang::gc::StressConfig every_major;
    every_major.collect_every_n_instructions = 1;
    schedules.push_back({"major_every_instruction", every_major});

    lang::gc::StressConfig every_minor;
    every_minor.collect_minor_every_n_instructions = 1;
    schedules.push_back({"minor_every_instruction", every_minor});

    lang::gc::StressConfig after_barrier;
    after_barrier.collect_before_every_allocation = true;
    after_barrier.collect_minor_after_every_write_barrier = true;
    schedules.push_back({"minor_after_every_barrier", after_barrier});

    lang::gc::StressConfig combined;
    combined.collect_before_every_allocation = true;
    combined.collect_after_every_allocation = true;
    combined.collect_every_n_instructions = 1;
    combined.collect_minor_every_n_instructions = 1;
    combined.collect_minor_after_every_write_barrier = true;
    schedules.push_back({"combined", combined});

    return schedules;
}

std::string execute_observable(const lang::Function& function, const Schedule& schedule) {
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    const auto result = vm.execute(function);
    return observable(vm, result);
}

void source_programs_are_gc_timing_equivalent() {
    const std::vector<std::string> sources = {
        R"SRC(
let seed: pair = pair(0, 0);
let holder: pair = pair(seed, seed);
let i: i64 = 0;
while i < 4 {
  holder.left = pair(i, seed);
  i = i + 1;
}
holder.left
)SRC",
        R"SRC(
let tail: pair = pair(10, 11);
let head: pair = tail;
let i: i64 = 0;
while i < 4 {
  head = pair(i, head);
  i = i + 1;
}
head
)SRC",
        R"SRC(
let seed: pair = pair(0, 0);
let a: pair = pair(seed, seed);
let b: pair = pair(seed, seed);
a.left = b;
b.right = a;
a
)SRC",
    };

    const auto schedules = stress_schedules();
    for (const auto& source : sources) {
        const auto compiled = require_compiles(source);
        const auto baseline = execute_observable(*compiled.function, schedules.front());
        for (const auto& schedule : schedules) {
            const auto observed = schedule.name == std::string(schedules.front().name)
                                      ? baseline
                                      : execute_observable(*compiled.function, schedule);
            require(observed == baseline,
                    std::string("source-level GC timing mismatch under ") + schedule.name +
                        "\n" + source_listing(source) + "baseline:\n" + baseline +
                        "\nobserved:\n" + observed + "\n");
        }
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
        {"rejects_undefined_variable_with_position",
         "type checker reports undefined names with exact source position",
         "RED before implementation: frontend exposes only type_check_placeholder",
         rejects_undefined_variable_with_position},
        {"rejects_plus_operand_type_mismatch_with_position",
         "operator + accepts only i64 operands",
         "RED before implementation: no lexer/parser/type checker pipeline",
         rejects_plus_operand_type_mismatch_with_position},
        {"rejects_less_operand_type_mismatch_with_position",
         "operator < accepts only i64 operands and returns bool",
         "RED before implementation: no lexer/parser/type checker pipeline",
         rejects_less_operand_type_mismatch_with_position},
        {"rejects_if_condition_type_mismatch_with_position",
         "if/else conditions must be bool because JumpIfFalse consumes bool",
         "RED before implementation: no typed control-flow checker",
         rejects_if_condition_type_mismatch_with_position},
        {"rejects_assignment_type_mismatch_with_position",
         "local assignment preserves declared static type",
         "RED before implementation: no local environment or assignment checker",
         rejects_assignment_type_mismatch_with_position},
        {"rejects_field_access_on_non_pair_with_position",
         "field access is restricted to pair values",
         "RED before implementation: no pair field checker",
         rejects_field_access_on_non_pair_with_position},
        {"rejects_field_assignment_type_mismatch_with_position",
         "field assignment preserves the tracked pair-field type",
         "RED before implementation: no pair field mutation checker",
         rejects_field_assignment_type_mismatch_with_position},
        {"executes_scalar_loop_end_to_end",
         "while loops compile to verifier-safe bytecode and execute on the VM",
         "RED before implementation: compiler does not exist",
         executes_scalar_loop_end_to_end},
        {"executes_if_else_end_to_end",
         "if/else branches compile as stack-neutral blocks",
         "RED before implementation: compiler does not exist",
         executes_if_else_end_to_end},
        {"executes_pair_field_access_and_mutation_end_to_end",
         "pair construction, field read, and field write execute through VM opcodes",
         "RED before implementation: compiler does not exist",
         executes_pair_field_access_and_mutation_end_to_end},
        {"executes_loop_building_linked_structure_end_to_end",
         "loop-carried pair locals agree with verifier object-site joins",
         "RED before implementation: compiler does not exist",
         executes_loop_building_linked_structure_end_to_end},
        {"executes_pair_cycle_created_by_field_assignment",
         "source field assignment can build cycles without bypassing heap barriers",
         "RED before implementation: compiler does not exist",
         executes_pair_cycle_created_by_field_assignment},
        {"compiled_type_checked_programs_pass_verifier_with_stack_maps",
         "well-typed source compiles only to bytecode accepted by verify_with_stack_maps",
         "RED before implementation: no compile-then-verify assertion",
         compiled_type_checked_programs_pass_verifier_with_stack_maps},
        {"source_programs_are_gc_timing_equivalent",
         "source programs produce identical observables under deterministic GC stress modes",
         "RED before implementation: no source-to-bytecode compiler",
         source_programs_are_gc_timing_equivalent},
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
        std::cerr << failures << " frontend pipeline test(s) failed\n";
        return 1;
    }
    return 0;
}
