#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"
#include "fuzz_common.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::ostringstream out;
        out << "expected source to compile\n" << source << "\n";
        for (const auto& diagnostic : compiled.diagnostics) {
            out << diagnostic.position.line << ":" << diagnostic.position.column
                << " " << diagnostic.message << "\n";
        }
        throw std::runtime_error(out.str());
    }
    require(compiled.verified_module.has_value(),
            "successful compile did not return a VerifiedModule");
    return compiled;
}

lang::Value execute_source(const std::string& source, lang::VM& vm) {
    auto compiled = require_compiles(source);
    return vm.execute(*compiled.verified_module);
}

std::string execute_string_source(const std::string& source,
                                  const fuzz::Schedule& schedule) {
    auto compiled = require_compiles(source);
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    const auto result = vm.execute(*compiled.verified_module);
    require(result.is_object(), "string program did not return an object");
    const auto bytes = vm.heap().string_bytes(result.as_object());
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::int64_t execute_i64_source(const std::string& source,
                                const fuzz::Schedule& schedule) {
    auto compiled = require_compiles(source);
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    return vm.execute(*compiled.verified_module).as_i64();
}

void require_diagnostic(const std::string& source, std::size_t line,
                        std::size_t column, const std::string& text) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected source rejection\n" + source);
    for (const auto& diagnostic : compiled.diagnostics) {
        if (diagnostic.position.line == line &&
            diagnostic.position.column == column &&
            diagnostic.message.find(text) != std::string::npos) {
            return;
        }
    }
    std::ostringstream observed;
    for (const auto& diagnostic : compiled.diagnostics) {
        observed << diagnostic.position.line << ":" << diagnostic.position.column
                 << " " << diagnostic.message << "\n";
    }
    throw std::runtime_error("missing positioned diagnostic containing '" + text +
                             "' at " + std::to_string(line) + ":" +
                             std::to_string(column) + "\n" + observed.str());
}

void frontend_accepts_all_three_statement_forms() {
    const std::string source = R"SRC(
let numbers: [i64] = [3, 4];
let entries: map<i64, i64> = map<i64, i64>();
entries[7] = 8;
let total: i64 = 0;
for value in numbers {
  total = total + value;
}
for key, value in entries {
  total = total + key + value;
}
for index in 1..4 {
  total = total + index;
}
total
)SRC";
    lang::VM vm;
    require(execute_source(source, vm).as_i64() == 28,
            "for-in forms returned the wrong result");
}

void frontend_reports_for_in_misuse_with_positions() {
    require_diagnostic("for x in 1 { }\n0\n", 1, 10,
                       "for-in requires array, map, or range");
    require_diagnostic(
        "let a: [i64] = [1];\nfor x, y in a { }\n0\n", 2, 8,
        "array iteration requires one loop variable");
    require_diagnostic(
        "let m: map<i64, i64> = map<i64, i64>();\nfor x in m { }\n0\n",
        2, 5, "map iteration requires two loop variables");
    require_diagnostic("for i in true..3 { }\n0\n", 1, 10,
                       "range lower bound must be i64");
    require_diagnostic("for i in 0..false { }\n0\n", 1, 13,
                       "range upper bound must be i64");
    require_diagnostic("for i in 0..2 {\n  i = 1;\n}\n0\n", 2, 3,
                       "cannot assign to immutable loop variable 'i'");
    require_diagnostic(
        "let a: [i64] = [1];\nfor x in a { x = 2; }\n0\n", 2, 14,
        "cannot assign to immutable loop variable 'x'");
    require_diagnostic(
        "let x: i64 = 0;\nfor x in 0..2 { }\n0\n", 2, 5,
        "loop variable 'x' conflicts with an existing local");
    require_diagnostic(
        "let m: map<i64, i64> = map<i64, i64>();\nfor x, x in m { }\n0\n",
        2, 8, "loop variable 'x' conflicts with another loop variable");
    require_diagnostic(
        "let a: [i64] = [1];\nlet w: weak<[i64]> = weak(a);\nlet maybe: [i64] = w.get();\nfor x in maybe { }\n0\n",
        4, 10, "for-in requires non-nil array or map");
    require_diagnostic(
        "let m: map<i64, i64> = map<i64, i64>();\nlet w: weak<map<i64, i64>> = weak(m);\nlet maybe: map<i64, i64> = w.get();\nfor k, v in maybe { }\n0\n",
        4, 13, "for-in requires non-nil array or map");
}

void moving_gc_forwards_ref_array_container_and_elements() {
    const std::string source = R"SRC(
let pairs: [pair<i64, i64>] = [pair(1, 10), pair(2, 20), pair(3, 30)];
let total: i64 = 0;
for item in pairs {
  total = total + item.left + item.right;
}

total
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64_source(source, schedule) == 66,
                std::string("ref-array iteration drifted under ") +
                    schedule.name);
    }
}

void bare_pair_array_iteration_preserves_element_facts() {
    const std::string source = R"SRC(
let values: [pair] = [pair(20, 22)];
let total: i64 = 0;
for item in values {
  total = total + item.left + item.right;
}
total
)SRC";
    lang::VM vm;
    require(execute_source(source, vm).as_i64() == 42,
            "for-in lost legacy bare-pair array element facts");
}

void scalar_bool_array_recovers_element_type() {
    const std::string source = R"SRC(
let flags: [bool] = [true, false, true];
let count: i64 = 0;
for flag in flags {
  if flag {
    count = count + 1;
  } else { }
}
count
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64_source(source, schedule) == 2,
                std::string("bool array iteration drifted under ") +
                    schedule.name);
    }
}

void map_iteration_observes_insertion_order_under_movement() {
    const std::string source = R"SRC(
let entries: map<str, i64> = map<str, i64>();
entries["third"] = 3;
entries["first"] = 1;
entries["second"] = 2;
let order: str = "";
for key, value in entries {
  order = order + key;
}
order
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_string_source(source, schedule) ==
                    "thirdfirstsecond",
                std::string("map insertion order drifted under ") +
                    schedule.name);
    }
}

void mutation_during_iteration_has_fixed_semantics() {
    const std::string array_source = R"SRC(
let values: [i64] = [1, 2, 3];
let total: i64 = 0;
let step: i64 = 0;
for value in values {
  if step < 1 {
    values[1] = 9;
  } else { }
  total = total + value;
  step = step + 1;
}

total
)SRC";
    const std::string map_update_source = R"SRC(
let entries: map<i64, i64> = map<i64, i64>();
entries[1] = 10;
entries[2] = 20;
let total: i64 = 0;
let step: i64 = 0;
for key, value in entries {
  if step < 1 {
    entries[2] = 99;
  } else { }
  total = total + value;
  step = step + 1;
}
total
)SRC";
    const std::string range_source = R"SRC(
let high: i64 = 3;
let count: i64 = 0;
for value in 0..high {
  high = 10;
  count = count + 1;
}
count
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64_source(array_source, schedule) == 13,
                std::string("array write-through was not visible under ") +
                    schedule.name);
        require(execute_i64_source(map_update_source, schedule) == 109,
                std::string("map update was not visible under ") +
                    schedule.name);
        require(execute_i64_source(range_source, schedule) == 3,
                std::string("range upper snapshot drifted under ") +
                    schedule.name);
    }
}

void range_bounds_evaluate_once_left_to_right() {
    const std::string source = R"SRC(
fn lower(state: map<i64, i64>) -> i64 {
  state[0] = state[0] + 1;
  0
}

fn upper(state: map<i64, i64>) -> i64 {
  state[0] = state[0] + 1;
  state[0]
}

let state: map<i64, i64> = map<i64, i64>();
state[0] = 0;
let count: i64 = 0;
for value in lower(state)..upper(state) {
  count = count + 1;
}
count + state[0]
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64_source(source, schedule) == 4,
                std::string("range bound evaluation drifted under ") +
                    schedule.name);
    }
}

void map_new_key_insertion_traps_deterministically() {
    const std::string source = R"SRC(
let entries: map<i64, i64> = map<i64, i64>();
entries[1] = 10;
for key, value in entries {
  entries[2] = 20;
}
0
)SRC";
    auto compiled = require_compiles(source);
    for (const auto& schedule : fuzz::schedules()) {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        bool trapped = false;
        try {
            (void)vm.execute(*compiled.verified_module);
        } catch (const std::out_of_range& error) {
            trapped = std::string(error.what()) ==
                      "map entry index out of bounds";
        }
        require(trapped, std::string("map insertion trap drifted under ") +
                             schedule.name);
    }
}

void empty_containers_and_empty_ranges_execute_zero_iterations() {
    const std::string source = R"SRC(
let values: [i64] = array<i64>(0, 7);
let entries: map<i64, i64> = map<i64, i64>();
let count: i64 = 0;
for value in values { count = count + 1; }
for key, value in entries { count = count + 1; }
for value in 4..4 { count = count + 1; }
for value in 5..2 { count = count + 1; }
count
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64_source(source, schedule) == 0,
                std::string("empty iteration executed under ") + schedule.name);
    }
}

void nested_map_of_arrays_survives_maximum_stress() {
    const std::string source = R"SRC(
let groups: map<str, [pair<i64, i64>]> = map<str, [pair<i64, i64>]>();
groups["b"] = [pair(1, 2), pair(3, 4)];
groups["a"] = [pair(5, 6)];
let total: i64 = 0;
for name, items in groups {
  for item in items {
    total = total + item.left + item.right;
  }
}
total
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64_source(source, schedule) == 21,
                std::string("nested map/array iteration drifted under ") +
                    schedule.name);
    }
}

void closures_capture_each_loop_iterations_value() {
    const std::string source = R"SRC(
let initial: fn() -> i64 = fn() -> i64 { 0 };
let closures: [fn() -> i64] = [initial, initial, initial];
let slot: i64 = 0;
for value in 4..7 {
  closures[slot] = fn() -> i64 { value };
  slot = slot + 1;
}
closures[0]() + closures[1]() + closures[2]()
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64_source(source, schedule) == 15,
                std::string("loop closure snapshot drifted under ") +
                    schedule.name);
    }
    require_diagnostic("for value in 0..1 { }\nvalue\n", 2, 1,
                       "undefined variable 'value'");
}

void hidden_reference_locals_have_exact_stack_map_bits() {
    const auto compiled = require_compiles(R"SRC(
let values: [pair<i64, i64>] = [pair(1, 2)];
let total: i64 = 0;
for value in values {
  total = total + value.left;
}
total
)SRC");
    const auto& function = compiled.verified_module->module().functions[0];
    require(function.local_count == 6,
            "array for-in did not allocate loop/index/bound/container locals");
    for (const auto& map : function.stack_maps) {
        require(map.local_object_slots.size() == function.local_count,
                "generated stack map omitted ordinary local root bits");
    }
    bool saw_backedge = false;
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        const auto& instruction = function.code[pc];
        if (instruction.op != lang::OpCode::Jump || instruction.operand < 0 ||
            static_cast<std::size_t>(instruction.operand) >= pc) {
            continue;
        }
        saw_backedge = true;
        const auto& header_map =
            function.stack_maps[static_cast<std::size_t>(instruction.operand)];
        require(header_map.local_object_slots[5],
                "hidden array container is not a precise loop-boundary root");
        require(header_map.local_object_slots[2],
                "reference loop variable is not a precise backedge root");
    }
    require(saw_backedge, "for-in lowering did not emit a verifier-visible backedge");
}

lang::VerifierReason first_reason(const lang::Module& module) {
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value() && !report.diagnostics.empty(),
            "expected verifier rejection with a diagnostic");
    return report.diagnostics.front().reason;
}

lang::Module positional_map_module(lang::OpCode operation) {
    lang::Module module;
    module.entry_function = 0;
    module.map_layouts.push_back(lang::MapLayout{
        lang::signature_value(lang::ValueKind::Int64),
        lang::signature_value(lang::ValueKind::Int64), false, false});
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.code = {{lang::OpCode::AllocMap, 0},
                  {lang::OpCode::ConstantI64, 0},
                  {operation, 0},
                  {lang::OpCode::Return, 0}};
    module.functions.push_back(std::move(entry));
    return module;
}

void verifier_and_vm_guard_lowered_loop_boundaries() {
    for (const auto operation : {lang::OpCode::MapKeyAt,
                                 lang::OpCode::MapValueAt}) {
        auto module = positional_map_module(operation);
        auto verified = lang::verify_module(module);
        require(verified.has_value(), "verifier rejected valid positional map access");
        lang::VM vm;
        bool trapped = false;
        try {
            (void)vm.execute(*verified);
        } catch (const std::out_of_range& error) {
            trapped = std::string(error.what()) ==
                      "map entry index out of bounds";
        }
        require(trapped, "map positional OOB diagnostic drifted");
    }

    auto confused = positional_map_module(lang::OpCode::MapKeyAt);
    confused.functions[0].code.insert(confused.functions[0].code.begin() + 2,
                                      {lang::OpCode::ConstantI64, 1});
    confused.functions[0].code.insert(confused.functions[0].code.begin() + 3,
                                      {lang::OpCode::ConstantI64, 2});
    confused.functions[0].code.insert(confused.functions[0].code.begin() + 4,
                                      {lang::OpCode::LessI64, 0});
    confused.functions[0].code.erase(confused.functions[0].code.begin() + 1);
    require(first_reason(confused) ==
                lang::VerifierReason::BadMapPositionAccess,
            "type-confused hidden index used an unstable verifier reason");

    lang::Module bad_preamble;
    bad_preamble.entry_function = 0;
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.local_count = 1;
    entry.code = {{lang::OpCode::ConstantI64, 0},
                  {lang::OpCode::StoreLocal, 0},
                  {lang::OpCode::Jump, 1},
                  {lang::OpCode::LoadLocal, 0},
                  {lang::OpCode::Return, 0}};
    bad_preamble.functions.push_back(std::move(entry));
    require(first_reason(bad_preamble) ==
                lang::VerifierReason::StackHeightMergeMismatch,
            "jump into lowered preamble used an unstable verifier reason");
}

std::string generate_loop_source(std::uint64_t seed) {
    const auto first = static_cast<std::int64_t>(seed % 9) + 1;
    const auto second = static_cast<std::int64_t>((seed * 3) % 9) + 1;
    const auto third = static_cast<std::int64_t>((seed * 5) % 9) + 1;
    const auto lower = static_cast<std::int64_t>(seed % 3);
    const auto upper = lower + 3;
    std::ostringstream out;
    out << "let values: [i64] = [" << first << ", " << second << ", "
        << third << "];\n"
        << "let groups: map<str, [i64]> = map<str, [i64]>();\n"
        << "groups[\"right\"] = [" << second << ", " << third << "];\n"
        << "groups[\"left\"] = [" << first << "];\n"
        << "let total: i64 = 0;\n"
        << "let while_step: i64 = 0;\n"
        << "while while_step < 4 {\n"
        << "  while_step = while_step + 1;\n"
        << "  if while_step < 2 { continue; } else { }\n"
        << "  total = total + while_step;\n"
        << "  if while_step < 4 { } else { break; }\n"
        << "}\n"
        << "let array_step: i64 = 0;\n"
        << "for value in values {\n"
        << "  array_step = array_step + 1;\n"
        << "  if array_step < 2 { continue; } else { }\n"
        << "  total = total + value;\n"
        << "  if array_step < 3 { } else { break; }\n"
        << "}\n"
        << "let group_step: i64 = 0;\n"
        << "for key, items in groups {\n"
        << "  group_step = group_step + 1;\n"
        << "  if group_step < 2 { continue; } else { }\n"
        << "  for item in items { total = total + item; continue; }\n"
        << "  break;\n"
        << "}\n"
        << "for index in " << lower << ".." << upper << " {\n"
        << "  if index < " << lower + 1 << " { continue; } else { }\n"
        << "  total = total + index;\n"
        << "  if index < " << upper - 1 << " { } else { break; }\n"
        << "}\n"
        << "print(to_str(total));\n"
        << "pair(total, values)\n";
    return out.str();
}

lang::VerifiedModule compile_loop_fuzz_source(std::uint64_t seed) {
    const auto source = generate_loop_source(seed);
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(), "loop source fuzz rejected seed=" +
                               std::to_string(seed) + "\n" + source);
    const auto reverified =
        lang::verify_with_diagnostics(compiled.verified_module->module());
    require(reverified.result.has_value(),
            "loop source fuzz produced verifier-rejected bytecode seed=" +
                std::to_string(seed));
    return *compiled.verified_module;
}

void run_loop_seed_schedule(std::uint64_t seed,
                            const fuzz::Schedule& schedule) {
    const auto verified = compile_loop_fuzz_source(seed);
    const auto schedules = fuzz::schedules();
    const auto baseline = fuzz::execute_once(
        verified, fuzz::find_schedule(schedules, "no_stress"));
    const auto observed = std::string(schedule.name) == "no_stress"
                              ? baseline
                              : fuzz::execute_once(verified, schedule);
    require(baseline.ok && observed.ok,
            "loop source fuzz trapped seed=" + std::to_string(seed) +
                " schedule=" + schedule.name + " baseline=" +
                baseline.error + " observed=" + observed.error);
    require(fuzz::same_observables(baseline, observed),
            "loop source fuzz drift seed=" + std::to_string(seed) +
                " schedule=" + schedule.name + "\nbaseline:\n" +
                baseline.observable + "\nobserved:\n" +
                observed.observable + "\nbaseline output bytes:\n" +
                fuzz::render_output_bytes(baseline.output) +
                "\nobserved output bytes:\n" +
                fuzz::render_output_bytes(observed.output));
}

std::vector<std::string> loop_mutants(std::uint64_t seed) {
    const auto bound = static_cast<std::int64_t>(seed % 5) + 1;
    return {
        "let values: [i64] = [1];\nfor x, y in values { }\n0\n",
        "for value in 0.." + std::to_string(bound) +
            " { value = 2; }\n0\n",
        "for value in false.." + std::to_string(bound) + " { }\n0\n",
        "let scalar: i64 = " + std::to_string(bound) +
            ";\nfor value in scalar { }\n0\n",
        "break;\n0\n",
        "continue;\n0\n",
        "let values: [i64] = [1];\n"
        "let weak_values: weak<[i64]> = weak(values);\n"
        "let maybe: [i64] = weak_values.get();\n"
        "while true {\n"
        "  if is_nil(maybe) { break; } else { maybe = values; break; }\n"
        "}\n"
        "maybe[0]\n",
    };
}

void require_loop_mutant_rejected(std::uint64_t seed, std::size_t mutant) {
    const auto mutants = loop_mutants(seed);
    require(mutant < mutants.size(), "loop mutant index out of range");
    const auto compiled = lang::frontend::compile_program(mutants[mutant]);
    require(!compiled.ok(), "loop mutant unexpectedly compiled seed=" +
                                std::to_string(seed) + " mutant=" +
                                std::to_string(mutant) + "\n" +
                                mutants[mutant]);
}

void loop_source_fuzz_pinned_snapshot() {
    // Iteration 30 deliberately extends this isolated, non-legacy grammar.
    // SHA-256: iteration 28 65fa15f9feef21c09a55a182b5049ff1b30bbbff68a8f74905e7c81184427219
    // SHA-256: iteration 30 82ba21140ada7e7e9ca1f4f38165c086679d24534f790d7c72c9ac1ca1eb0e30
    const std::string expected = R"SRC(let values: [i64] = [2, 4, 6];
let groups: map<str, [i64]> = map<str, [i64]>();
groups["right"] = [4, 6];
groups["left"] = [2];
let total: i64 = 0;
let while_step: i64 = 0;
while while_step < 4 {
  while_step = while_step + 1;
  if while_step < 2 { continue; } else { }
  total = total + while_step;
  if while_step < 4 { } else { break; }
}
let array_step: i64 = 0;
for value in values {
  array_step = array_step + 1;
  if array_step < 2 { continue; } else { }
  total = total + value;
  if array_step < 3 { } else { break; }
}
let group_step: i64 = 0;
for key, items in groups {
  group_step = group_step + 1;
  if group_step < 2 { continue; } else { }
  for item in items { total = total + item; continue; }
  break;
}
for index in 1..4 {
  if index < 2 { continue; } else { }
  total = total + index;
  if index < 3 { } else { break; }
}
print(to_str(total));
pair(total, values)
)SRC";
    require(generate_loop_source(28) == expected,
            "loop source pinned snapshot changed\nactual:\n" +
                generate_loop_source(28));
}

void loop_source_fuzz_corpus_and_mutants() {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 10,
            "loop source fuzz requires exactly ten deterministic schedules");
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        for (const auto& schedule : schedules) {
            run_loop_seed_schedule(seed, schedule);
        }
        for (std::size_t mutant = 0; mutant < loop_mutants(seed).size();
             ++mutant) {
            require_loop_mutant_rejected(seed, mutant);
        }
    }
}

using Test = std::pair<const char*, void (*)()>;

} // namespace

int main(int argc, char** argv) {
    try {
        const auto schedules = fuzz::schedules();
        if (argc == 6 && std::string(argv[1]) == "--replay" &&
            std::string(argv[2]) == "--seed" &&
            std::string(argv[4]) == "--schedule") {
            const auto seed = fuzz::parse_seed(argv[3]);
            const auto& schedule = fuzz::find_schedule(schedules, argv[5]);
            run_loop_seed_schedule(seed, schedule);
            std::cerr << "[PASS] loops replay seed=" << seed
                      << " schedule=" << schedule.name << "\n";
            return 0;
        }
        if (argc == 6 && std::string(argv[1]) == "--replay" &&
            std::string(argv[2]) == "--seed" &&
            std::string(argv[4]) == "--mutant") {
            const auto seed = fuzz::parse_seed(argv[3]);
            const auto mutant =
                static_cast<std::size_t>(fuzz::parse_seed(argv[5]));
            require_loop_mutant_rejected(seed, mutant);
            std::cerr << "[PASS] loops mutant replay seed=" << seed
                      << " mutant=" << mutant << "\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--dump-corpus" &&
            std::string(argv[2]) == "loops") {
            for (std::uint64_t seed = 1; seed <= 10; ++seed) {
                std::cout << "grammar=loops seed=" << seed << "\n"
                          << generate_loop_source(seed);
            }
            return 0;
        }
        if (argc != 1) {
            std::cerr << "usage: " << argv[0]
                      << " --replay --seed <uint64> --schedule <name>\n"
                      << "       " << argv[0]
                      << " --replay --seed <uint64> --mutant <index>\n"
                      << "       " << argv[0]
                      << " --dump-corpus loops\n";
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }

    const std::vector<Test> tests{
        {"frontend_accepts_all_three_statement_forms",
         frontend_accepts_all_three_statement_forms},
        {"frontend_reports_for_in_misuse_with_positions",
         frontend_reports_for_in_misuse_with_positions},
        {"moving_gc_forwards_ref_array_container_and_elements",
         moving_gc_forwards_ref_array_container_and_elements},
        {"bare_pair_array_iteration_preserves_element_facts",
         bare_pair_array_iteration_preserves_element_facts},
        {"scalar_bool_array_recovers_element_type",
         scalar_bool_array_recovers_element_type},
        {"map_iteration_observes_insertion_order_under_movement",
         map_iteration_observes_insertion_order_under_movement},
        {"mutation_during_iteration_has_fixed_semantics",
         mutation_during_iteration_has_fixed_semantics},
        {"range_bounds_evaluate_once_left_to_right",
         range_bounds_evaluate_once_left_to_right},
        {"map_new_key_insertion_traps_deterministically",
         map_new_key_insertion_traps_deterministically},
        {"empty_containers_and_empty_ranges_execute_zero_iterations",
         empty_containers_and_empty_ranges_execute_zero_iterations},
        {"nested_map_of_arrays_survives_maximum_stress",
         nested_map_of_arrays_survives_maximum_stress},
        {"closures_capture_each_loop_iterations_value",
         closures_capture_each_loop_iterations_value},
        {"hidden_reference_locals_have_exact_stack_map_bits",
         hidden_reference_locals_have_exact_stack_map_bits},
        {"verifier_and_vm_guard_lowered_loop_boundaries",
         verifier_and_vm_guard_lowered_loop_boundaries},
        {"loop_source_fuzz_pinned_snapshot",
         loop_source_fuzz_pinned_snapshot},
        {"loop_source_fuzz_corpus_and_mutants",
         loop_source_fuzz_corpus_and_mutants},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cerr << "[PASS] " << name << "\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
