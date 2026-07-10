#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "fuzz_common.hpp"

#include <cstddef>
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
    out << "source:\n" << source;
    if (source.empty() || source.back() != '\n') {
        out << "\n";
    }
    return out.str();
}

std::string diagnostics_listing(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.position.line << ":" << diagnostic.position.column << " "
            << diagnostic.message << "\n";
    }
    return out.str();
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        throw std::runtime_error("expected source to compile\n" +
                                 source_listing(source) + "diagnostics:\n" +
                                 diagnostics_listing(compiled.diagnostics));
    }
    require(compiled.verified_module.has_value(),
            "successful source compile did not return a VerifiedModule\n" +
                source_listing(source));
    return compiled;
}

void require_diagnostic(const std::string& source,
                        const std::string& expected_message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected source to be rejected\n" + source_listing(source));
    require(!compiled.diagnostics.empty(),
            "rejected source did not include diagnostics\n" + source_listing(source));
    const auto rendered = diagnostics_listing(compiled.diagnostics);
    require(rendered.find(expected_message) != std::string::npos,
            "diagnostics did not contain expected message '" + expected_message +
                "'\n" + source_listing(source) + "diagnostics:\n" + rendered);
}

std::vector<fuzz::Schedule> source_array_schedules() {
    std::vector<fuzz::Schedule> schedules;
    schedules.push_back({"no_stress", {}});

    lang::gc::StressConfig every_instruction;
    every_instruction.collect_every_n_instructions = 1;
    every_instruction.collect_minor_every_n_instructions = 1;
    schedules.push_back({"major_and_minor_every_instruction", every_instruction});

    lang::gc::StressConfig after_barrier;
    after_barrier.collect_before_every_allocation = true;
    after_barrier.collect_after_every_allocation = true;
    after_barrier.collect_minor_after_every_write_barrier = true;
    schedules.push_back({"after_every_barrier", after_barrier});

    lang::gc::StressConfig combined;
    combined.collect_before_every_allocation = true;
    combined.collect_after_every_allocation = true;
    combined.collect_every_n_instructions = 1;
    combined.collect_minor_every_n_instructions = 1;
    combined.collect_minor_after_every_write_barrier = true;
    schedules.push_back({"combined", combined});
    return schedules;
}

void require_same_observable_under_stress(const std::string& source,
                                          const std::string& expected) {
    const auto compiled = require_compiles(source);
    const auto schedules = source_array_schedules();
    const auto baseline = fuzz::execute_once(*compiled.verified_module, schedules.front());
    require(baseline.ok, "baseline execution trapped: " + baseline.error + "\n" +
                             source_listing(source));
    require(baseline.observable == expected,
            "baseline observable mismatch\nexpected:\n" + expected +
                "\nobserved:\n" + baseline.observable + "\n" + source_listing(source));

    for (const auto& schedule : schedules) {
        const auto observed = schedule.name == std::string(schedules.front().name)
                                  ? baseline
                                  : fuzz::execute_once(*compiled.verified_module,
                                                       schedule);
        require(observed.ok,
                std::string("execution trapped under ") + schedule.name + ": " +
                    observed.error + "\n" + source_listing(source));
        require(fuzz::same_observables(baseline, observed),
                std::string("observable mismatch under ") + schedule.name +
                    "\nbaseline:\n" + baseline.observable +
                    "\nobserved:\n" + observed.observable + "\n" +
                    "baseline output bytes:\n" +
                    fuzz::render_output_bytes(baseline.output) +
                    "\nobserved output bytes:\n" +
                    fuzz::render_output_bytes(observed.output) + "\n" +
                    source_listing(source));
    }
}

std::size_t count_opcode(const lang::VerifiedModule& verified, lang::OpCode op) {
    std::size_t count = 0;
    for (const auto& function : verified.module().functions) {
        for (const auto& instruction : function.code) {
            if (instruction.op == op) {
                ++count;
            }
        }
    }
    return count;
}

void scalar_i64_arrays_loop_sum_under_gc_stress() {
    const std::string source = R"SRC(
let values: [i64] = array<i64>(4, 0);
let i: i64 = 0;
while i < values.len {
  values[i] = i + 1;
  i = i + 1;
}
let sum: i64 = 0;
i = 0;
while i < values.len {
  sum = sum + values[i];
  i = i + 1;
}
sum
)SRC";

    require_same_observable_under_stress(source, "i64:10");
}

void bool_arrays_use_scalar_payload_and_recover_bool_elements() {
    const std::string source = R"SRC(
let flags: [bool] = [true, false, true];
let score: i64 = 0;
if flags[0] {
  score = score + 5;
} else {
  score = score + 50;
}
if flags[1] {
  score = score + 70;
} else {
  score = score + 2;
}
score
)SRC";

    require_same_observable_under_stress(source, "i64:7");
}

void ref_array_of_pairs_recovers_pair_element_type_after_indexing() {
    const std::string source = R"SRC(
let pairs: [pair<i64, i64>] = array<pair<i64, i64>>(2, pair(1, 2));
pairs[1] = pair(40, 3);
pairs[1].left + pairs[0].right
)SRC";

    require_same_observable_under_stress(source, "i64:42");
}

void nested_arrays_are_ref_arrays_of_scalar_arrays() {
    const std::string source = R"SRC(
let first: [i64] = [5, 6];
let second: [i64] = array<i64>(2, 0);
second[1] = 36;
let grid: [[i64]] = array<[i64]>(2, first);
grid[1] = second;
grid[0][1] + grid[1][1]
)SRC";

    require_same_observable_under_stress(source, "i64:42");
}

void compiler_emits_scalar_or_ref_array_opcodes_by_element_type() {
    const std::string source = R"SRC(
let ints: [i64] = array<i64>(2, 0);
let bools: [bool] = [true, false];
let pairs: [pair<i64, i64>] = array<pair<i64, i64>>(1, pair(1, 2));
let nested: [[i64]] = array<[i64]>(1, ints);
ints[1] = 41;
pairs[0] = pair(40, 2);
nested[0] = ints;
ints[1] + pairs[0].right + nested[0][0]
)SRC";

    const auto compiled = require_compiles(source);
    const auto& verified = *compiled.verified_module;
    require(count_opcode(verified, lang::OpCode::AllocArray) >= 2,
            "scalar [i64]/[bool] arrays did not compile to AllocArray\n" +
                source_listing(source));
    require(count_opcode(verified, lang::OpCode::ArrayGet) >= 2,
            "scalar array indexing did not compile to ArrayGet\n" +
                source_listing(source));
    require(count_opcode(verified, lang::OpCode::ArraySet) >= 1,
            "scalar array store did not compile to ArraySet\n" +
                source_listing(source));
    require(count_opcode(verified, lang::OpCode::AllocRefArray) >= 2,
            "reference element arrays did not compile to AllocRefArray\n" +
                source_listing(source));
    require(count_opcode(verified, lang::OpCode::RefArrayGet) >= 2,
            "reference array indexing did not compile to RefArrayGet\n" +
                source_listing(source));
    require(count_opcode(verified, lang::OpCode::RefArraySet) >= 2,
            "reference array store did not compile to RefArraySet\n" +
                source_listing(source));
    require(verified.verification().functions.size() == verified.module().functions.size(),
            "compile boundary did not return module-level verifier proof");
}

void rejects_array_element_type_mismatch() {
    require_diagnostic("let xs: [i64] = [1, true]; xs[0]\n",
                       "array literal elements must have one type");
}

void rejects_non_i64_array_index() {
    require_diagnostic("let xs: [i64] = [1, 2]; xs[true]\n",
                       "array index must be i64");
}

void rejects_len_on_non_array() {
    require_diagnostic("let x: i64 = 1; x.len\n", "len requires array");
}

void rejects_index_on_non_array() {
    require_diagnostic("let x: i64 = 1; x[0]\n", "indexing requires array");
}

void rejects_nil_reference_array_initializer() {
    require_diagnostic(
        "type List = pair<i64, List>;\nlet xs: [List] = array<List>(2, nil); xs.len\n",
        "reference array elements must be non-nil");
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
        {"scalar_i64_arrays_loop_sum_under_gc_stress",
         "[i64] sized construction, len, index, store, and loop iteration use ScalarArray",
         "BASELINE-RED on f8dca63: source parser has no array syntax",
         scalar_i64_arrays_loop_sum_under_gc_stress},
        {"bool_arrays_use_scalar_payload_and_recover_bool_elements",
         "[bool] stores encoded scalar payload and indexing recovers bool for branches",
         "BASELINE-RED on f8dca63: frontend has no [bool] array representation",
         bool_arrays_use_scalar_payload_and_recover_bool_elements},
        {"ref_array_of_pairs_recovers_pair_element_type_after_indexing",
         "[pair<i64,i64>] indexes recover pair field type and stores use RefArraySet",
         "BASELINE-RED on f8dca63: RefArrayGet result is not exposed to source",
         ref_array_of_pairs_recovers_pair_element_type_after_indexing},
        {"nested_arrays_are_ref_arrays_of_scalar_arrays",
         "[[i64]] stores scalar-array references and xs[i][j] recovers nested element types",
         "BASELINE-RED on f8dca63: source type grammar has no nested arrays",
         nested_arrays_are_ref_arrays_of_scalar_arrays},
        {"compiler_emits_scalar_or_ref_array_opcodes_by_element_type",
         "compile boundary enforces scalar/reference representation split in verified bytecode",
         "BASELINE-RED on f8dca63: compiler cannot emit array opcodes from source",
         compiler_emits_scalar_or_ref_array_opcodes_by_element_type},
        {"rejects_array_element_type_mismatch",
         "array literals reject out-of-type elements before bytecode generation",
         "BASELINE-RED on f8dca63: array literals do not parse",
         rejects_array_element_type_mismatch},
        {"rejects_non_i64_array_index",
         "array indexes must type-check as i64",
         "BASELINE-RED on f8dca63: array indexing does not parse",
         rejects_non_i64_array_index},
        {"rejects_len_on_non_array",
         ".len is only defined on array values",
         "BASELINE-RED on f8dca63: .len is not a source postfix",
         rejects_len_on_non_array},
        {"rejects_index_on_non_array",
         "indexing is only defined on arrays",
         "BASELINE-RED on f8dca63: indexing is not a source postfix",
         rejects_index_on_non_array},
        {"rejects_nil_reference_array_initializer",
         "RefArray construction cannot create observable nil holes",
         "BASELINE-RED on f8dca63: source has no RefArray construction",
         rejects_nil_reference_array_initializer},
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
        std::cerr << failures << " iteration-22 source array test(s) failed\n";
        return 1;
    }
    return 0;
}
