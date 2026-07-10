#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"
#include "fuzz_common.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
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
            "successful compile omitted VerifiedModule");
    return compiled;
}

std::int64_t execute_i64(const std::string& source,
                         const fuzz::Schedule& schedule) {
    auto compiled = require_compiles(source);
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    return vm.execute(*compiled.verified_module).as_i64();
}

std::string execute_string(const std::string& source,
                           const fuzz::Schedule& schedule) {
    auto compiled = require_compiles(source);
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    const auto result = vm.execute(*compiled.verified_module);
    require(result.is_object(), "expected string result object");
    const auto bytes = vm.heap().string_bytes(result.as_object());
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void require_diagnostic(const std::string& source, std::size_t line,
                        std::size_t column, const std::string& message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected source rejection\n" + source);
    for (const auto& diagnostic : compiled.diagnostics) {
        if (diagnostic.position.line == line &&
            diagnostic.position.column == column &&
            diagnostic.message == message) {
            return;
        }
    }
    std::ostringstream observed;
    for (const auto& diagnostic : compiled.diagnostics) {
        observed << diagnostic.position.line << ":" << diagnostic.position.column
                 << " " << diagnostic.message << "\n";
    }
    throw std::runtime_error("missing stable diagnostic '" + message + "' at " +
                             std::to_string(line) + ":" +
                             std::to_string(column) + "\n" + observed.str());
}

void require_diagnostic_contains(const std::string& source, std::size_t line,
                                 std::size_t column,
                                 const std::string& message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected source rejection\n" + source);
    for (const auto& diagnostic : compiled.diagnostics) {
        if (diagnostic.position.line == line &&
            diagnostic.position.column == column &&
            diagnostic.message.find(message) != std::string::npos) {
            return;
        }
    }
    std::ostringstream observed;
    for (const auto& diagnostic : compiled.diagnostics) {
        observed << diagnostic.position.line << ":" << diagnostic.position.column
                 << " " << diagnostic.message << "\n";
    }
    throw std::runtime_error("missing diagnostic containing '" + message +
                             "' at " + std::to_string(line) + ":" +
                             std::to_string(column) + "\n" + observed.str());
}

void parses_loop_control_and_rejects_outside_loops() {
    const std::string source = R"SRC(
let total: i64 = 0;
while total < 5 {
  total = total + 1;
  if total < 2 { continue; } else { }
  if total < 4 { } else { break; }
}
total
)SRC";
    const auto& schedule = fuzz::find_schedule(fuzz::schedules(), "no_stress");
    require(execute_i64(source, schedule) == 4,
            "while break/continue returned the wrong result");

    require_diagnostic("break;\n0\n", 1, 1,
                       "'break' is only allowed inside a loop");
    require_diagnostic("continue;\n0\n", 1, 1,
                       "'continue' is only allowed inside a loop");
    require_diagnostic(R"SRC(let initial: fn() -> i64 = fn() -> i64 { 0 };
let closures: [fn() -> i64] = [initial];
for index in 0..1 {
  closures[0] = fn() -> i64 {
    break;
    0
  };
}
0
)SRC",
                       5, 5, "'break' is only allowed inside a loop");

    const std::string unreachable_after_break = R"SRC(
let values: [i64] = [1];
while true {
  break;
  values[0] = 9;
}
values[0]
)SRC";
    require(execute_i64(unreachable_after_break, schedule) == 1,
            "unreachable statement after break affected flow or lowering");
}

bool is_increment_preamble(const std::vector<lang::Instruction>& code,
                           std::size_t target) {
    return target + 3 < code.size() &&
           code[target].op == lang::OpCode::LoadLocal &&
           code[target + 1].op == lang::OpCode::ConstantI64 &&
           code[target + 1].operand == 1 &&
           code[target + 2].op == lang::OpCode::AddI64 &&
           code[target + 3].op == lang::OpCode::StoreLocal &&
           code[target].operand == code[target + 3].operand;
}

std::size_t require_single_forward_jump(const lang::Function& function,
                                        const std::string& context) {
    std::vector<std::size_t> jumps;
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        const auto& instruction = function.code[pc];
        if (instruction.op == lang::OpCode::Jump && instruction.operand >= 0 &&
            static_cast<std::size_t>(instruction.operand) > pc) {
            jumps.push_back(pc);
        }
    }
    require(jumps.size() == 1,
            context + " expected exactly one forward Jump but found " +
                std::to_string(jumps.size()));
    return jumps.front();
}

void compiler_targets_each_loop_boundary_exactly() {
    const std::vector<std::string> for_sources{
        "for index in 0..3 { continue; }\n0\n",
        "let values: [i64] = [1, 2, 3];\n"
        "for value in values { continue; }\n0\n",
        "let entries: map<str, i64> = map<str, i64>();\n"
        "entries[\"a\"] = 1;\nfor key, value in entries { continue; }\n0\n",
    };
    for (const auto& source : for_sources) {
        const auto compiled = require_compiles(source);
        const auto& function = compiled.verified_module->module().functions[0];
        const auto jump_pc = require_single_forward_jump(function, "for continue");
        const auto target =
            static_cast<std::size_t>(function.code[jump_pc].operand);
        require(is_increment_preamble(function.code, target),
                "for continue did not target hidden index increment preamble");
        const auto backedge = target + 4;
        require(backedge < function.code.size() &&
                    function.code[backedge].op == lang::OpCode::Jump,
                "for increment preamble was not followed by a backedge");
        if (source.find("map<") != std::string::npos) {
            const auto header = static_cast<std::size_t>(
                function.code[backedge].operand);
            require(header + 2 < function.code.size() &&
                        function.code[header].op == lang::OpCode::LoadLocal &&
                        function.code[header + 1].op == lang::OpCode::LoadLocal &&
                        function.code[header + 2].op == lang::OpCode::MapLen,
                    "map continue backedge skipped the mutation-trap check");
        }
    }

    const auto while_compiled = require_compiles(R"SRC(
let index: i64 = 0;
while index < 3 {
  index = index + 1;
  continue;
}
index
)SRC");
    const auto& while_function =
        while_compiled.verified_module->module().functions[0];
    std::vector<std::size_t> backward_jumps;
    for (std::size_t pc = 0; pc < while_function.code.size(); ++pc) {
        const auto& instruction = while_function.code[pc];
        if (instruction.op == lang::OpCode::Jump && instruction.operand >= 0 &&
            static_cast<std::size_t>(instruction.operand) < pc) {
            backward_jumps.push_back(pc);
        }
    }
    require(backward_jumps.size() == 1,
            "while continue did not emit one condition backedge");
    const auto while_target = static_cast<std::size_t>(
        while_function.code[backward_jumps.front()].operand);
    require(while_function.code[while_target].op == lang::OpCode::LoadLocal,
            "while continue did not target condition header");

    for (const auto& source : {
             std::string("while true { break; }\n0\n"),
             std::string("for index in 0..3 { break; }\n0\n"),
             std::string("let values: [i64] = [1];\n"
                         "for value in values { break; }\n0\n"),
             std::string("let entries: map<str, i64> = map<str, i64>();\n"
                         "entries[\"a\"] = 1;\n"
                         "for key, value in entries { break; }\n0\n")}) {
        const auto compiled = require_compiles(source);
        const auto& function = compiled.verified_module->module().functions[0];
        const auto jump_pc = require_single_forward_jump(function, "loop break");
        const auto target =
            static_cast<std::size_t>(function.code[jump_pc].operand);
        require(target < function.code.size() &&
                    function.code[target].op == lang::OpCode::ConstantI64,
                "break did not target the first post-loop instruction");
    }
}

void continue_preserves_every_for_in_step() {
    const std::string source = R"SRC(
let values: [i64] = [10, 11, 12, 13, 14];
let entries: map<str, i64> = map<str, i64>();
entries["a"] = 20;
entries["b"] = 21;
entries["c"] = 22;
entries["d"] = 23;
entries["e"] = 24;
let array_even: bool = true;
let map_even: bool = true;
let range_even: bool = true;
let array_sum: i64 = 0;
let map_sum: i64 = 0;
let range_sum: i64 = 0;
let visits: i64 = 0;
for value in values {
  if array_even {
    array_even = false;
    continue;
  } else {
    array_even = true;
  }
  array_sum = array_sum + value;
  visits = visits + 1;
}
for key, value in entries {
  if map_even {
    map_even = false;
    continue;
  } else {
    map_even = true;
  }
  map_sum = map_sum + value;
  visits = visits + 1;
}
for index in 0..5 {
  if range_even {
    range_even = false;
    continue;
  } else {
    range_even = true;
  }
  range_sum = range_sum + index;
  visits = visits + 1;
}
array_sum + map_sum + range_sum + visits
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64(source, schedule) == 78,
                std::string("continue changed for-in step discipline under ") +
                    schedule.name);
    }

    const std::string map_growth = R"SRC(
let entries: map<str, i64> = map<str, i64>();
entries["a"] = 1;
for key, value in entries {
  entries["new"] = 2;
  continue;
}
0
)SRC";
    const auto compiled = require_compiles(map_growth);
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
        require(trapped, std::string("continue skipped map growth trap under ") +
                             schedule.name);
    }
}

void inner_break_exits_once_and_preserves_closure_snapshots() {
    const std::string source = R"SRC(
let initial: fn() -> i64 = fn() -> i64 { 0 };
let closures: [fn() -> i64] = [initial, initial, initial];
let slot: i64 = 0;
let inner_visits: i64 = 0;
for outer in 0..3 {
  for inner in 10..20 {
    closures[slot] = fn() -> i64 { outer + inner };
    inner_visits = inner_visits + 1;
    break;
  }
  slot = slot + 1;
}
closures[0]() + closures[1]() + closures[2]() + inner_visits
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64(source, schedule) == 36,
                std::string("nested break/capture drifted under ") +
                    schedule.name);
    }
}

void break_exit_flow_uses_only_reaching_paths() {
    const std::string positive = R"SRC(
let values: [i64] = [40, 2];
let weak_values: weak<[i64]> = weak(values);
let maybe: [i64] = weak_values.get();
while true {
  if is_nil(maybe) {
    maybe = values;
    break;
  } else {
    maybe = values;
    break;
  }
}
maybe[0] + maybe[1]
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64(positive, schedule) == 42,
                std::string("break exit lost definite non-nil fact under ") +
                    schedule.name);
    }

    const std::string negative = R"SRC(
let values: [i64] = [40, 2];
let weak_values: weak<[i64]> = weak(values);
let maybe: [i64] = weak_values.get();
while true {
  if is_nil(maybe) {
    break;
  } else {
    maybe = values;
    continue;
  }
}
maybe[0]
)SRC";
    require_diagnostic_contains(negative, 13, 6,
                                "requires non-nil value of type [i64]");
}

void while_break_continue_matches_for_in_under_stress() {
    const std::string source = R"SRC(
let index: i64 = 0;
let total: i64 = 0;
while index < 6 {
  index = index + 1;
  if index < 3 { continue; } else { }
  total = total + index;
  if total < 7 { } else { break; }
}
index + total
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64(source, schedule) == 11,
                std::string("while break/continue drifted under ") +
                    schedule.name);
    }
}

lang::VerifierReason first_reason(const lang::Module& module) {
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value() && !report.diagnostics.empty(),
            "expected verifier rejection with diagnostic");
    return report.diagnostics.front().reason;
}

lang::Module scalar_module(std::vector<lang::Instruction> code,
                           std::uint32_t local_count = 0) {
    lang::Module module;
    module.entry_function = 0;
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.local_count = local_count;
    entry.code = std::move(code);
    module.functions.push_back(std::move(entry));
    return module;
}

void verifier_rejects_corrupted_loop_edges_with_existing_reasons() {
    const auto compiled_break =
        require_compiles("for index in 0..2 { break; }\n0\n");
    auto into_preamble = compiled_break.verified_module->module();
    auto& break_function = into_preamble.functions[0];
    for (auto& function : into_preamble.functions) {
        function.stack_maps.clear();
    }
    const auto break_jump =
        require_single_forward_jump(break_function, "corrupted break");
    require(break_function.code.size() > 1 &&
                break_function.code[1].op == lang::OpCode::StoreLocal,
            "range for-in setup preamble shape changed");
    break_function.code[break_jump].operand = 1;
    require(first_reason(into_preamble) ==
                lang::VerifierReason::StackHeightMergeMismatch,
            "break edge into for-in preamble changed verifier reason");

    const auto skipped_increment = scalar_module(
        {{lang::OpCode::ConstantI64, 0},
         {lang::OpCode::StoreLocal, 0},
         {lang::OpCode::LoadLocal, 0},
         {lang::OpCode::ConstantI64, 3},
         {lang::OpCode::LessI64, 0},
         {lang::OpCode::JumpIfFalse, 11},
         {lang::OpCode::Jump, 2},
         {lang::OpCode::LoadLocal, 0},
         {lang::OpCode::ConstantI64, 1},
         {lang::OpCode::AddI64, 0},
         {lang::OpCode::StoreLocal, 0},
         {lang::OpCode::ConstantI64, 0},
         {lang::OpCode::Return, 0}},
        1);
    require(first_reason(skipped_increment) ==
                lang::VerifierReason::UnreachableCode,
            "continue edge skipping hidden increment changed verifier reason");

    const auto confused_break = scalar_module(
        {{lang::OpCode::ConstantI64, 0},
         {lang::OpCode::ConstantI64, 1},
         {lang::OpCode::LessI64, 0},
         {lang::OpCode::JumpIfFalse, 6},
         {lang::OpCode::ConstantI64, 99},
         {lang::OpCode::Jump, 6},
         {lang::OpCode::ConstantI64, 0},
         {lang::OpCode::Return, 0}});
    require(first_reason(confused_break) ==
                lang::VerifierReason::StackHeightMergeMismatch,
            "stack-height-confused break changed verifier reason");
}

std::pair<std::size_t, std::size_t> loop_control_targets(
    const lang::Function& function) {
    std::optional<std::size_t> continue_target;
    std::optional<std::size_t> break_target;
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        const auto& instruction = function.code[pc];
        if (instruction.op != lang::OpCode::Jump || instruction.operand < 0 ||
            static_cast<std::size_t>(instruction.operand) <= pc) {
            continue;
        }
        const auto target = static_cast<std::size_t>(instruction.operand);
        if (is_increment_preamble(function.code, target)) {
            continue_target = target;
        } else {
            break_target = target;
        }
    }
    require(continue_target.has_value() && break_target.has_value(),
            "could not identify break and continue target pcs");
    return {*continue_target, *break_target};
}

void require_exact_target_map(const lang::Function& function,
                              std::size_t target,
                              const std::vector<bool>& local_bits,
                              const std::string& context) {
    require(target < function.stack_maps.size(),
            context + " target omitted stack map");
    const auto& map = function.stack_maps[target];
    require(map.object_slots.empty(),
            context + " target operand stack was not empty");
    if (map.local_object_slots != local_bits) {
        std::ostringstream out;
        out << context << " target local root bits were not exact\nexpected:";
        for (const bool bit : local_bits) {
            out << " " << bit;
        }
        out << "\nactual:";
        for (const bool bit : map.local_object_slots) {
            out << " " << bit;
        }
        throw std::runtime_error(out.str());
    }
}

void gc_at_branch_targets_preserves_exact_roots() {
    const std::string ref_array_source = R"SRC(
let values: [pair<i64, i64>] = [pair(1, 10), pair(2, 20), pair(3, 30)];
let chosen: pair<i64, i64> = pair(0, 0);
for value in values {
  chosen = value;
  if value.left < 2 { continue; } else { break; }
}
chosen.left + chosen.right
)SRC";
    const auto ref_compiled = require_compiles(ref_array_source);
    const auto& ref_function =
        ref_compiled.verified_module->module().functions[0];
    const auto [ref_continue, ref_break] = loop_control_targets(ref_function);
    // Local 3 is the pre-existing RefArray literal construction temporary;
    // for-in then owns scalar index/bound locals 4/5 and container root 6.
    const std::vector<bool> ref_bits{
        true, true, true, true, false, false, true};
    require_exact_target_map(ref_function, ref_continue, ref_bits,
                             "ref-array continue");
    require_exact_target_map(ref_function, ref_break, ref_bits,
                             "ref-array break");

    const std::string map_source = R"SRC(
let entries: map<str, pair<i64, i64>> = map<str, pair<i64, i64>>();
entries["a"] = pair(1, 10);
entries["b"] = pair(2, 20);
entries["c"] = pair(3, 30);
let chosen_key: str = "none";
let chosen_value: pair<i64, i64> = pair(0, 0);
for key, value in entries {
  chosen_key = key;
  chosen_value = value;
  if value.left < 2 { continue; } else { break; }
}
chosen_key + to_str(chosen_value.left)
)SRC";
    const auto map_compiled = require_compiles(map_source);
    const auto& map_function =
        map_compiled.verified_module->module().functions[0];
    const auto [map_continue, map_break] = loop_control_targets(map_function);
    const std::vector<bool> map_bits{
        true, true, true, true, true, false, false, true};
    require_exact_target_map(map_function, map_continue, map_bits,
                             "map continue");
    require_exact_target_map(map_function, map_break, map_bits,
                             "map break");

    const auto schedules = fuzz::schedules();
    for (const auto* name : {"major_every_1", "minor_every_1"}) {
        const auto& schedule = fuzz::find_schedule(schedules, name);
        require(execute_i64(ref_array_source, schedule) == 22,
                std::string("ref-array roots drifted at branch targets under ") +
                    name);
        require(execute_string(map_source, schedule) == "b2",
                std::string("map roots drifted at branch targets under ") +
                    name);
    }
}

using Test = std::pair<const char*, void (*)()>;

} // namespace

int main() {
    const std::vector<Test> tests{
        {"parses_loop_control_and_rejects_outside_loops",
         parses_loop_control_and_rejects_outside_loops},
        {"compiler_targets_each_loop_boundary_exactly",
         compiler_targets_each_loop_boundary_exactly},
        {"continue_preserves_every_for_in_step",
         continue_preserves_every_for_in_step},
        {"inner_break_exits_once_and_preserves_closure_snapshots",
         inner_break_exits_once_and_preserves_closure_snapshots},
        {"break_exit_flow_uses_only_reaching_paths",
         break_exit_flow_uses_only_reaching_paths},
        {"while_break_continue_matches_for_in_under_stress",
         while_break_continue_matches_for_in_under_stress},
        {"verifier_rejects_corrupted_loop_edges_with_existing_reasons",
         verifier_rejects_corrupted_loop_edges_with_existing_reasons},
        {"gc_at_branch_targets_preserves_exact_roots",
         gc_at_branch_targets_preserves_exact_roots},
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
