#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"
#include "fuzz_common.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
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
            "successful compile did not return a VerifiedModule");
    return compiled;
}

std::int64_t execute_i64(const std::string& source) {
    const auto compiled = require_compiles(source);
    lang::VM vm;
    return vm.execute(*compiled.verified_module).as_i64();
}

std::int64_t execute_i64(const std::string& source,
                         const fuzz::Schedule& schedule) {
    const auto compiled = require_compiles(source);
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    return vm.execute(*compiled.verified_module).as_i64();
}

bool execute_bool(const std::string& source,
                  const fuzz::Schedule& schedule) {
    const auto compiled = require_compiles(source);
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    return vm.execute(*compiled.verified_module).as_bool();
}

std::pair<std::int64_t, std::string> execute_i64_with_output(
    const std::string& source, const fuzz::Schedule& schedule) {
    const auto compiled = require_compiles(source);
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    const auto value = vm.execute(*compiled.verified_module).as_i64();
    return {value, std::string(vm.output().begin(), vm.output().end())};
}

void require_diagnostic(const std::string& source, std::size_t line,
                        std::size_t column, const std::string& message) {
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
        observed << diagnostic.position.line << ":"
                 << diagnostic.position.column << " " << diagnostic.message
                 << "\n";
    }
    throw std::runtime_error("missing diagnostic containing '" + message +
                             "' at " + std::to_string(line) + ":" +
                             std::to_string(column) + "\n" + observed.str());
}

void labeled_loop_syntax_targets_lexically_enclosing_loops() {
    const std::string while_source = R"SRC(
let total: i64 = 0;
outer: while total < 8 {
  inner: while true {
    total = total + 1;
    if total < 3 { continue outer; } else { break outer; }
  }
}
total
)SRC";
    require(execute_i64(while_source) == 3,
            "labeled while control targeted the wrong loop");

    const std::string range_source = R"SRC(
let visits: i64 = 0;
outer: for i in 0..4 {
  inner: for j in 0..3 {
    visits = visits + 1;
    continue outer;
  }
}
visits
)SRC";
    require(execute_i64(range_source) == 4,
            "labeled range continue bypassed the outer increment");

    const std::string nearest_source = R"SRC(
let visits: i64 = 0;
outer: for i in 0..3 {
  inner: for j in 0..3 {
    visits = visits + 1;
    break;
  }
}
visits
)SRC";
    require(execute_i64(nearest_source) == 3,
            "unlabeled break stopped targeting the nearest loop");
}

void label_errors_have_stable_positioned_diagnostics() {
    require_diagnostic("while true { break missing; }\n0\n", 1, 20,
                       "unknown loop label 'missing'");

    require_diagnostic(R"SRC(done: while false { }
while true { break done; }
0
)SRC",
                       2, 20,
                       "loop label 'done' does not lexically enclose this break");

    require_diagnostic(R"SRC(outer: while true {
  outer: for i in 0..1 { break; }
  break;
}
0
)SRC",
                       2, 3,
                       "loop label 'outer' duplicates an active loop label");

    require_diagnostic("break nowhere;\n0\n", 1, 7,
                       "unknown loop label 'nowhere'");
    require_diagnostic("continue nowhere;\n0\n", 1, 10,
                       "unknown loop label 'nowhere'");

    require_diagnostic("not_a_loop: let value: i64 = 1;\nvalue\n", 1, 1,
                       "loop label must precede 'while' or 'for'");
}

void labeled_continue_preserves_outer_for_in_steps() {
    const std::string source = R"SRC(
let values: [i64] = [10, 11, 12];
let entries: map<str, i64> = map<str, i64>();
entries["a"] = 1;
entries["b"] = 2;
let visits: i64 = 0;
range_loop: for i in 0..4 {
  inner_range: while true {
    visits = visits + 1;
    continue range_loop;
  }
}
array_loop: for value in values {
  inner_array: while true {
    visits = visits + 1;
    continue array_loop;
  }
}
map_loop: for key, value in entries {
  inner_map: while true {
    visits = visits + 1;
    continue map_loop;
  }
}
visits
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64(source, schedule) == 9,
                std::string("labeled continue changed outer step under ") +
                    schedule.name);
    }

    const std::string map_growth = R"SRC(
let entries: map<str, i64> = map<str, i64>();
entries["a"] = 1;
outer: for key, value in entries {
  inner: while true {
    entries["new"] = 2;
    continue outer;
  }
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
            trapped =
                std::string(error.what()) == "map entry index out of bounds";
        }
        require(trapped,
                std::string("outer continue skipped map mutation trap under ") +
                    schedule.name);
    }
}

void labels_are_callable_local_and_active_names_do_not_shadow() {
    require_diagnostic(R"SRC(let initial: fn() -> i64 = fn() -> i64 { 0 };
let closures: [fn() -> i64] = [initial];
outer: while true {
  closures[0] = fn() -> i64 {
    break outer;
    0
  };
  break;
}
0
)SRC",
                       5, 11, "unknown loop label 'outer'");

    require_diagnostic(R"SRC(done: while false { }
while true {
  continue done;
}
0
)SRC",
                       3, 12,
                       "loop label 'done' does not lexically enclose this continue");

    const std::string reusable_sibling = R"SRC(
same: for i in 0..1 { break same; }
same: for i in 0..1 { break same; }
0
)SRC";
    require(execute_i64(reusable_sibling) == 0,
            "sibling loops could not reuse an inactive label");

    const std::string lambda_owns_label = R"SRC(
let initial: fn() -> i64 = fn() -> i64 { 0 };
let closures: [fn() -> i64] = [initial];
outer: while true {
  closures[0] = fn() -> i64 {
    outer: while true { break outer; }
    42
  };
  break outer;
}
closures[0]()
)SRC";
    require(execute_i64(lambda_owns_label) == 42,
            "lambda-local label conflicted with caller label");
}

void multi_level_exit_flow_uses_only_reaching_states() {
    const std::string positive = R"SRC(
let values: [i64] = [40, 2];
let weak_values: weak<[i64]> = weak(values);
let maybe: [i64] = weak_values.get();
outer: while true {
  inner: while true {
    if is_nil(maybe) {
      maybe = values;
      break outer;
    } else {
      maybe = values;
      break outer;
    }
  }
}
maybe[0] + maybe[1]
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64(positive, schedule) == 42,
                std::string("outer break lost non-nil flow fact under ") +
                    schedule.name);
    }

    const std::string negative = R"SRC(
let values: [i64] = [40, 2];
let weak_values: weak<[i64]> = weak(values);
let maybe: [i64] = weak_values.get();
outer: while true {
  inner: while true {
    if is_nil(maybe) {
      break outer;
    } else {
      maybe = values;
      continue outer;
    }
  }
}
maybe[0]
)SRC";
    require_diagnostic(negative, 15, 6,
                       "requires non-nil value of type [i64]");
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

std::pair<std::size_t, std::size_t> labeled_outer_targets(
    const lang::Function& function, std::size_t source_line) {
    std::optional<std::size_t> continue_target;
    std::optional<std::size_t> break_target;
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        if (function.source_positions[pc].line != source_line ||
            function.code[pc].op != lang::OpCode::Jump ||
            function.code[pc].operand < 0) {
            continue;
        }
        const auto target =
            static_cast<std::size_t>(function.code[pc].operand);
        if (is_increment_preamble(function.code, target)) {
            continue_target = target;
        } else if (target > pc) {
            break_target = target;
        }
    }
    require(continue_target.has_value() && break_target.has_value(),
            "could not identify both outer labeled branch targets");
    return {*continue_target, *break_target};
}

void require_exact_target_map(const lang::Function& function,
                              std::size_t target,
                              const std::vector<bool>& expected,
                              const std::string& context) {
    require(target < function.stack_maps.size(),
            context + " target omitted a stack map");
    const auto& map = function.stack_maps[target];
    require(map.object_slots.empty(),
            context + " target operand stack was not empty");
    if (map.local_object_slots != expected) {
        std::ostringstream out;
        out << context << " local root bits differ\nexpected:";
        for (const bool bit : expected) {
            out << " " << bit;
        }
        out << "\nactual:";
        for (const bool bit : map.local_object_slots) {
            out << " " << bit;
        }
        throw std::runtime_error(out.str());
    }
}

void labeled_exit_cleans_catch_binding_before_outer_join() {
    const std::string source = R"SRC(
variant Error { Boom() }
fn fail() -> i64 { throw Error.Boom(); 0 }
let caught: i64 = 0;
outer: while true {
  try {
    caught = fail();
  } catch (problem: Error) {
    caught = 1;
    break outer;
  }
}
let problem: i64 = 41;
caught + problem
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64(source, schedule) == 42,
                std::string("catch binding leaked across labeled exit under ") +
                    schedule.name);
    }
}

void two_depth_branch_targets_have_exact_roots_under_movement() {
    const std::string source = R"SRC(
let values: [pair<i64, i64>] = [pair(1, 10), pair(2, 20), pair(3, 30)];
let chosen: pair<i64, i64> = pair(0, 0);
outer: for value in values {
  inner: for index in 0..2 {
    chosen = value;
    if value.left < 2 { continue outer; } else { break outer; }
  }
}
chosen.left + chosen.right
)SRC";
    const auto compiled = require_compiles(source);
    const auto& function =
        compiled.verified_module->module().functions.front();
    const auto [continue_target, break_target] =
        labeled_outer_targets(function, 7);
    const std::vector<bool> exact_bits{
        true, true, true, false, true, false, false, true, false, false};
    require_exact_target_map(function, continue_target, exact_bits,
                             "two-depth outer continue");
    require_exact_target_map(function, break_target, exact_bits,
                             "two-depth outer break");

    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64(source, schedule) == 22,
                std::string("two-depth roots drifted under ") +
                    schedule.name);
    }
}

void integer_builtins_are_total_except_abs_minimum() {
    const std::string source =
        "abs(-7) + abs(0) + abs(9) + min(8, -3) + max(-5, 12)\n";
    for (const auto& schedule : fuzz::schedules()) {
        require(execute_i64(source, schedule) == 25,
                std::string("integer builtin result drifted under ") +
                    schedule.name);
    }

    const std::string order_source = R"SRC(
fn first_value() -> i64 {
  print("left");
  7
}
fn second_value() -> i64 {
  print("right");
  3
}
min(first_value(), second_value()) + max(first_value(), second_value())
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        const auto [value, output] =
            execute_i64_with_output(order_source, schedule);
        require(value == 10, "min/max returned the wrong selected values");
        require(output == "left\nright\nleft\nright\n",
                std::string("min/max evaluation order drifted under ") +
                    schedule.name);
    }

    const std::string minimum =
        "abs(-9223372036854775808)\n";
    std::optional<std::string> baseline;
    for (const auto& schedule : fuzz::schedules()) {
        const auto compiled = require_compiles(minimum);
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        std::string observed;
        try {
            (void)vm.execute(*compiled.verified_module);
        } catch (const std::runtime_error& error) {
            observed = error.what();
        }
        require(observed.find("absolute value overflow") !=
                    std::string::npos,
                std::string("abs(INT64_MIN) used the wrong trap under ") +
                    schedule.name + ": " + observed);
        if (!baseline.has_value()) {
            baseline = observed;
        } else {
            require(observed == *baseline,
                    "abs(INT64_MIN) diagnostic/pc drifted across schedules");
        }
    }
}

void integer_builtin_errors_are_stable_and_positioned() {
    require_diagnostic("abs()\n", 1, 1,
                       "abs expects exactly 1 argument");
    require_diagnostic("abs(true)\n", 1, 5,
                       "abs argument expects i64");
    require_diagnostic("min(1)\n", 1, 1,
                       "min expects exactly 2 arguments");
    require_diagnostic("min(true, 1)\n", 1, 5,
                       "min argument expects i64");
    require_diagnostic("max(1, false)\n", 1, 8,
                       "max argument expects i64");
    require_diagnostic("max(1, 2, 3)\n", 1, 1,
                       "max expects exactly 2 arguments");
}

void string_search_builtins_follow_byte_index_conventions() {
    struct BoolCase {
        const char* source;
        bool expected;
    };
    const std::vector<BoolCase> bool_cases{
        {"\"banana\".contains(\"ana\")\n", true},
        {"\"banana\".contains(\"xyz\")\n", false},
        {"\"\".contains(\"\")\n", true},
        {"\"abc\".contains(\"\")\n", true},
        {"\"abc\".contains(\"abcd\")\n", false},
        {"\"banana\".starts_with(\"ban\")\n", true},
        {"\"banana\".starts_with(\"ana\")\n", false},
        {"\"banana\".ends_with(\"ana\")\n", true},
        {"\"banana\".ends_with(\"ban\")\n", false},
        {"\"\".starts_with(\"\")\n", true},
        {"\"\".ends_with(\"\")\n", true},
    };
    for (const auto& test : bool_cases) {
        for (const auto& schedule : fuzz::schedules()) {
            require(execute_bool(test.source, schedule) == test.expected,
                    std::string("string search bool drifted under ") +
                        schedule.name + "\n" + test.source);
        }
    }

    const std::vector<std::pair<const char*, std::int64_t>> index_cases{
        {"\"banana\".index_of(\"ana\")\n", 1},
        {"\"aaaa\".index_of(\"aa\")\n", 0},
        {"\"banana\".index_of(\"xyz\")\n", -1},
        {"\"abc\".index_of(\"\")\n", 0},
        {"\"\".index_of(\"\")\n", 0},
        {"\"\".index_of(\"x\")\n", -1},
        {"\"aéz\".index_of(\"é\")\n", 1},
    };
    for (const auto& [source, expected] : index_cases) {
        for (const auto& schedule : fuzz::schedules()) {
            require(execute_i64(source, schedule) == expected,
                    std::string("index_of byte index drifted under ") +
                        schedule.name + "\n" + source);
        }
    }

    const std::string order_source = R"SRC(
fn haystack() -> str {
  print("receiver");
  "banana"
}
fn needle() -> str {
  print("argument");
  "ana"
}
haystack().index_of(needle())
)SRC";
    for (const auto& schedule : fuzz::schedules()) {
        const auto [value, output] =
            execute_i64_with_output(order_source, schedule);
        require(value == 1, "index_of exactly-once witness returned wrong index");
        require(output == "receiver\nargument\n",
                std::string("string method evaluation order drifted under ") +
                    schedule.name);
    }
}

void string_search_errors_are_stable_and_positioned() {
    require_diagnostic("\"x\".contains()\n", 1, 5,
                       "contains expects exactly 1 argument");
    require_diagnostic("\"x\".index_of(\"x\", \"y\")\n", 1, 5,
                       "index_of expects exactly 1 argument");
    require_diagnostic("\"x\".starts_with(1)\n", 1, 17,
                       "starts_with argument expects str");
    require_diagnostic("\"x\".ends_with(false)\n", 1, 15,
                       "ends_with argument expects str");
    require_diagnostic("1.contains(\"x\")\n", 1, 3,
                       "contains requires str receiver");

    require_diagnostic(R"SRC(let text: str = "abc";
let weak_text: weak<str> = weak(text);
let maybe: str = weak_text.get();
maybe.index_of("a")
)SRC",
                       4, 7, "index_of requires non-nil str receiver");
}

void abs_opcode_is_append_only_and_verifier_typed() {
    require(static_cast<int>(lang::OpCode::I64Abs) ==
                static_cast<int>(lang::OpCode::StrIntern) + 1,
            "I64Abs was not appended after the iteration-44 opcode tail");

    lang::Module module;
    module.entry_function = 0;
    module.string_constants = {"not an integer"};
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.code = {{lang::OpCode::PushStr, 0},
                  {lang::OpCode::I64Abs, 0},
                  {lang::OpCode::Return, 0}};
    module.functions.push_back(std::move(entry));
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value() && !report.diagnostics.empty(),
            "verifier accepted I64Abs with a string operand");
    require(report.diagnostics.front().reason ==
                lang::VerifierReason::I64AbsRequiresI64,
            "I64Abs type confusion used the wrong verifier reason");
}

void string_search_allocation_maps_keep_only_operands_rooted() {
    const std::vector<std::string> sources{
        "\"banana\".contains(\"ana\")\n",
        "\"banana\".index_of(\"ana\")\n",
        "\"banana\".starts_with(\"ban\")\n",
        "\"banana\".ends_with(\"ana\")\n",
    };
    for (const auto& source : sources) {
        const auto compiled = require_compiles(source);
        const auto& function =
            compiled.verified_module->module().functions.front();
        require(function.local_count == 5,
                "string search lowering did not reserve exactly five locals");
        std::size_t substring_count = 0;
        for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
            if (function.code[pc].op != lang::OpCode::StrSub) {
                continue;
            }
            ++substring_count;
            require(pc < function.stack_maps.size(),
                    "string search StrSub omitted stack map");
            const auto& map = function.stack_maps[pc];
            require(map.object_slots ==
                        std::vector<bool>({true, false, false}),
                    "StrSub stack map did not root only its receiver");
            require(map.local_object_slots ==
                        std::vector<bool>({true, true, false, false, false}),
                    "search lowering did not precisely root receiver and needle");
        }
        require(substring_count == 1,
                "fixed-size search lowering emitted an unexpected StrSub count");
    }
}

using Test = std::pair<const char*, void (*)()>;

} // namespace

int main() {
    const std::vector<Test> tests{
        {"labeled_loop_syntax_targets_lexically_enclosing_loops",
         labeled_loop_syntax_targets_lexically_enclosing_loops},
        {"label_errors_have_stable_positioned_diagnostics",
         label_errors_have_stable_positioned_diagnostics},
        {"labeled_continue_preserves_outer_for_in_steps",
         labeled_continue_preserves_outer_for_in_steps},
        {"labels_are_callable_local_and_active_names_do_not_shadow",
         labels_are_callable_local_and_active_names_do_not_shadow},
        {"multi_level_exit_flow_uses_only_reaching_states",
         multi_level_exit_flow_uses_only_reaching_states},
        {"labeled_exit_cleans_catch_binding_before_outer_join",
         labeled_exit_cleans_catch_binding_before_outer_join},
        {"two_depth_branch_targets_have_exact_roots_under_movement",
         two_depth_branch_targets_have_exact_roots_under_movement},
        {"integer_builtins_are_total_except_abs_minimum",
         integer_builtins_are_total_except_abs_minimum},
        {"integer_builtin_errors_are_stable_and_positioned",
         integer_builtin_errors_are_stable_and_positioned},
        {"string_search_builtins_follow_byte_index_conventions",
         string_search_builtins_follow_byte_index_conventions},
        {"string_search_errors_are_stable_and_positioned",
         string_search_errors_are_stable_and_positioned},
        {"abs_opcode_is_append_only_and_verifier_typed",
         abs_opcode_is_append_only_and_verifier_typed},
        {"string_search_allocation_maps_keep_only_operands_rooted",
         string_search_allocation_maps_keep_only_operands_rooted},
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
