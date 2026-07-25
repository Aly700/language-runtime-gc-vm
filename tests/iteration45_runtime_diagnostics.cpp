#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"

#include "fuzz_common.hpp"

#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

lang::VerifiedModule verify_or_throw(lang::Module module) {
    auto report = lang::verify_module_with_diagnostics(std::move(module));
    if (!report.module.has_value()) {
        std::ostringstream out;
        out << "expected module to verify";
        for (const auto& diagnostic : report.diagnostics) {
            out << "\n" << lang::format_verifier_diagnostic(diagnostic);
        }
        throw std::runtime_error(out.str());
    }
    return std::move(*report.module);
}

lang::frontend::CompileResult compile_or_throw(std::string_view source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::ostringstream out;
        out << "expected source to compile\n" << source;
        for (const auto& diagnostic : compiled.diagnostics) {
            out << "\n" << diagnostic.position.line << ":"
                << diagnostic.position.column << " " << diagnostic.message;
        }
        throw std::runtime_error(out.str());
    }
    return compiled;
}

template <typename Executable>
std::string execute_error(lang::VM& vm, const Executable& executable) {
    try {
        (void)vm.execute(executable);
    } catch (const std::exception& error) {
        return error.what();
    }
    throw std::runtime_error("execution unexpectedly succeeded");
}

void require_position(const std::optional<lang::DebugSourcePosition>& actual,
                      std::size_t line, std::size_t column,
                      const std::string& context) {
    require(actual.has_value(), context + " omitted source position");
    require(actual->line == line && actual->column == column,
            context + " source position mismatch: expected " +
                std::to_string(line) + ":" + std::to_string(column) +
                ", got " + std::to_string(actual->line) + ":" +
                std::to_string(actual->column));
}

void hand_built_module_trace_falls_back_to_index_and_pc() {
    lang::Module module;
    module.entry_function = 0;
    module.string_constants = {"bad"};
    module.functions.resize(1);
    auto& entry = module.functions.front();
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.code = {
        {lang::OpCode::PushStr, 0},
        {lang::OpCode::StrToI64, 0},
        {lang::OpCode::Return, 0},
    };

    auto verified = verify_or_throw(std::move(module));
    lang::VM vm;
    const auto message = execute_error(vm, verified);
    require(message ==
                "runtime trap at function 0 pc 1: invalid string for i64 conversion",
            "hand-built module trap message changed: " + message);

    const auto& trace = vm.last_trap_trace();
    require(trace.has_value(), "hand-built module omitted runtime trace");
    require(trace->kind == lang::RuntimeFailureKind::Trap,
            "hand-built trap used the wrong failure kind");
    require(!trace->exception_variant.has_value(),
            "ordinary trap unexpectedly named an exception variant");
    require(trace->frames.size() == 1,
            "hand-built trace did not contain exactly the entry frame");
    const auto& frame = trace->frames.front();
    require(frame.function_index == 0, "hand-built trace function index changed");
    require(frame.pc == 1, "hand-built trace pc changed");
    require(!frame.function_name.has_value(),
            "hand-built trace invented a function name");
    require(!frame.source_position.has_value(),
            "hand-built trace invented a source position");
}

void frontend_debug_tables_are_complete_and_deterministic() {
    constexpr std::string_view source =
        "fn fail(text: str) -> i64 {\n"
        "  to_i64(text)\n"
        "}\n"
        "fn middle(text: str) -> i64 {\n"
        "  fail(text)\n"
        "}\n"
        "fn outer() -> i64 {\n"
        "  middle(\"bad\")\n"
        "}\n"
        "outer()\n";

    const auto first = compile_or_throw(source);
    const auto second = compile_or_throw(source);
    const auto& first_module = first.verified_module->module();
    const auto& second_module = second.verified_module->module();

    require(first_module.functions.size() == 4,
            "frontend function ordering changed");
    require(second_module.functions.size() == first_module.functions.size(),
            "identical compiles changed function count");
    const std::vector<std::string> expected_names{
        "<entry>", "fail", "middle", "outer"};
    for (std::size_t index = 0; index < first_module.functions.size(); ++index) {
        const auto& first_function = first_module.functions[index];
        const auto& second_function = second_module.functions[index];
        require(first_function.debug_name == expected_names[index],
                "frontend function debug name mismatch at index " +
                    std::to_string(index));
        require(first_function.debug_name == second_function.debug_name,
                "identical compiles changed a function name");
        require(first_function.source_positions ==
                    second_function.source_positions,
                "identical compiles changed a pc-position table");
        require(first_function.source_positions.size() ==
                    first_function.code.size(),
                "frontend debug table does not cover every pc");
    }

    require(first_module.functions[1].code[1].op == lang::OpCode::StrToI64,
            "test source no longer lowers StrToI64 at fail pc 1");
    require(first_module.functions[1].source_positions[1] ==
                lang::DebugSourcePosition{2, 3},
            "StrToI64 debug position did not name its source token");
}

void nested_trace_normalizes_active_and_suspended_pcs() {
    constexpr std::string_view source =
        "fn fail(text: str) -> i64 {\n"
        "  to_i64(text)\n"
        "}\n"
        "fn middle(text: str) -> i64 {\n"
        "  fail(text)\n"
        "}\n"
        "fn outer() -> i64 {\n"
        "  middle(\"bad\")\n"
        "}\n"
        "outer()\n";
    auto compiled = compile_or_throw(source);

    lang::VM vm;
    const auto message = execute_error(vm, *compiled.verified_module);
    require(message ==
                "runtime trap at function 1 pc 1: invalid string for i64 conversion",
            "nested trap message changed: " + message);

    const auto& trace = vm.last_trap_trace();
    require(trace.has_value(), "nested trap omitted runtime trace");
    require(trace->frames.size() == 4,
            "nested trace omitted an active or suspended frame");

    struct ExpectedFrame {
        std::size_t function_index;
        std::size_t pc;
        const char* name;
        std::size_t line;
        std::size_t column;
    };
    constexpr ExpectedFrame expected[] = {
        {1, 1, "fail", 2, 3},
        {2, 1, "middle", 5, 3},
        {3, 1, "outer", 8, 3},
        {0, 0, "<entry>", 10, 1},
    };
    for (std::size_t i = 0; i < trace->frames.size(); ++i) {
        const auto& actual = trace->frames[i];
        require(actual.function_index == expected[i].function_index,
                "nested trace function index mismatch at frame " +
                    std::to_string(i));
        require(actual.pc == expected[i].pc,
                "nested trace pc mismatch at frame " + std::to_string(i));
        require(actual.function_name.has_value() &&
                    *actual.function_name == expected[i].name,
                "nested trace function name mismatch at frame " +
                    std::to_string(i));
        require_position(actual.source_position, expected[i].line,
                         expected[i].column,
                         "nested trace frame " + std::to_string(i));
    }
}

void successful_execution_clears_a_prior_trace() {
    lang::Module trapping_module;
    trapping_module.entry_function = 0;
    trapping_module.string_constants = {"bad"};
    trapping_module.functions.resize(1);
    auto& entry = trapping_module.functions.front();
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.code = {
        {lang::OpCode::PushStr, 0},
        {lang::OpCode::StrToI64, 0},
        {lang::OpCode::Return, 0},
    };
    const auto trapping = verify_or_throw(std::move(trapping_module));
    const auto succeeding = compile_or_throw("40 + 2\n");

    lang::VM vm;
    (void)execute_error(vm, trapping);
    require(vm.last_trap_trace().has_value(),
            "setup trap did not populate the side channel");
    const auto result = vm.execute(*succeeding.verified_module);
    require(result.tag() == lang::Value::Tag::Int64 &&
                result.as_i64() == 42,
            "successful reuse returned the wrong result");
    require(!vm.last_trap_trace().has_value(),
            "successful execution retained a stale trace");
}

void caught_exception_leaves_no_terminal_trace() {
    constexpr std::string_view source =
        "variant Fault { Boom() }\n"
        "fn fail() -> i64 {\n"
        "  throw Fault.Boom();\n"
        "  0\n"
        "}\n"
        "let answer: i64 = 0;\n"
        "try {\n"
        "  answer = fail();\n"
        "} catch (fault: Fault) {\n"
        "  answer = 42;\n"
        "}\n"
        "answer\n";
    auto compiled = compile_or_throw(source);

    lang::VM vm;
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Int64 &&
                result.as_i64() == 42,
            "caught exception program returned the wrong result");
    require(!vm.last_trap_trace().has_value(),
            "caught language exception populated the terminal trace");
}

void uncaught_exception_preserves_pre_unwind_frames_and_variant() {
    constexpr std::string_view source =
        "variant Fault { Boom(i64) }\n"
        "fn deepest() -> i64 {\n"
        "  throw Fault.Boom(7);\n"
        "  0\n"
        "}\n"
        "fn middle() -> i64 {\n"
        "  deepest()\n"
        "}\n"
        "fn outer() -> i64 {\n"
        "  middle()\n"
        "}\n"
        "outer()\n";
    auto compiled = compile_or_throw(source);

    lang::VM vm;
    const auto message = execute_error(vm, *compiled.verified_module);
    require(message == "uncaught exception Fault",
            "uncaught exception message changed: " + message);
    const auto& trace = vm.last_trap_trace();
    require(trace.has_value(), "uncaught exception omitted its trace");
    require(trace->kind == lang::RuntimeFailureKind::UncaughtException,
            "uncaught exception was classified as an ordinary trap");
    require(trace->exception_variant.has_value() &&
                *trace->exception_variant == "Fault",
            "uncaught exception trace omitted its variant name");
    require(trace->frames.size() == 4,
            "uncaught exception lost frames during unwind");

    struct ExpectedFrame {
        std::size_t function_index;
        std::size_t pc;
        const char* name;
        std::size_t line;
        std::size_t column;
    };
    constexpr ExpectedFrame expected[] = {
        {1, 2, "deepest", 3, 3},
        {2, 0, "middle", 7, 3},
        {3, 0, "outer", 10, 3},
        {0, 0, "<entry>", 12, 1},
    };
    for (std::size_t i = 0; i < trace->frames.size(); ++i) {
        const auto& actual = trace->frames[i];
        require(actual.function_index == expected[i].function_index,
                "uncaught trace function mismatch at frame " +
                    std::to_string(i));
        require(actual.pc == expected[i].pc,
                "uncaught trace pc mismatch at frame " +
                    std::to_string(i));
        require(actual.function_name.has_value() &&
                    *actual.function_name == expected[i].name,
                "uncaught trace name mismatch at frame " +
                    std::to_string(i));
        require_position(actual.source_position, expected[i].line,
                         expected[i].column,
                         "uncaught trace frame " + std::to_string(i));
    }
}

void uncaught_exception_preserves_a_suspended_closure_frame() {
    constexpr std::string_view source =
        "variant Fault { Boom() }\n"
        "fn invoke(action: fn() -> i64) -> i64 {\n"
        "  action()\n"
        "}\n"
        "let action: fn() -> i64 = fn() -> i64 {\n"
        "  throw Fault.Boom();\n"
        "  0\n"
        "};\n"
        "invoke(action)\n";
    auto compiled = compile_or_throw(source);

    lang::VM vm;
    vm.set_gc_stress(fuzz::find_schedule(
        fuzz::schedules(), "combined_mark_compact").stress);
    const auto message = execute_error(vm, *compiled.verified_module);
    require(message == "uncaught exception Fault",
            "closure exception message changed: " + message);

    const auto& trace = vm.last_trap_trace();
    require(trace.has_value(),
            "uncaught closure exception omitted its trace");
    require(trace->kind == lang::RuntimeFailureKind::UncaughtException &&
                trace->exception_variant ==
                    std::optional<std::string>{"Fault"},
            "uncaught closure exception lost its kind or variant");
    require(trace->frames.size() == 3,
            "uncaught closure exception lost a suspended frame");
    require(trace->frames[0].function_name ==
                std::optional<std::string>{"<lambda@5:27>"} &&
                trace->frames[1].function_name ==
                    std::optional<std::string>{"invoke"} &&
                trace->frames[2].function_name ==
                    std::optional<std::string>{"<entry>"},
            "uncaught closure trace names or order changed");
    require_position(trace->frames[0].source_position, 6, 3,
                     "uncaught closure active frame");
    require_position(trace->frames[1].source_position, 3, 3,
                     "uncaught closure suspended caller");
    require_position(trace->frames[2].source_position, 9, 1,
                     "uncaught closure entry caller");
}

struct FailureObservation {
    std::string message;
    std::string output;
    lang::RuntimeTrace trace;
};

FailureObservation observe_failure(
    const lang::VerifiedModule& module, const fuzz::Schedule& schedule,
    std::optional<std::size_t> max_call_depth = std::nullopt) {
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    if (max_call_depth.has_value()) {
        vm.set_max_call_depth(*max_call_depth);
    }
    try {
        (void)vm.execute(module);
    } catch (const std::exception& error) {
        require(vm.last_trap_trace().has_value(),
                std::string("trapping execution omitted trace under ") +
                    schedule.name);
        return FailureObservation{
            error.what(),
            std::string(vm.output().begin(), vm.output().end()),
            *vm.last_trap_trace()};
    }
    throw std::runtime_error(
        std::string("expected execution to fail under ") + schedule.name);
}

FailureObservation require_schedule_stable_failure(
    std::string_view name, std::string_view source,
    std::string_view expected_message,
    std::optional<std::size_t> max_call_depth = std::nullopt) {
    auto compiled = compile_or_throw(source);
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "shared stress schedule count changed from 15");
    const auto baseline = observe_failure(
        *compiled.verified_module, schedules.front(), max_call_depth);
    require(baseline.message == expected_message,
            std::string(name) + " legacy error text changed: " +
                baseline.message);

    for (std::size_t i = 1; i < schedules.size(); ++i) {
        const auto observed = observe_failure(
            *compiled.verified_module, schedules[i], max_call_depth);
        require(observed.message == baseline.message,
                std::string(name) + " error text changed under " +
                    schedules[i].name);
        require(observed.output == baseline.output,
                std::string(name) + " output bytes changed under " +
                    schedules[i].name);
        require(observed.trace == baseline.trace,
                std::string(name) + " trace changed under " +
                    schedules[i].name);
    }
    return baseline;
}

void trapping_matrix_has_identical_traces_across_all_schedules() {
    const auto scalar_array = require_schedule_stable_failure(
        "scalar_array_oob",
        "fn fail() -> i64 {\n"
        "  let values: [i64] = [10, 20];\n"
        "  values[4]\n"
        "}\n"
        "fail()\n",
        "scalar array index out of bounds");
    require(scalar_array.trace.frames.front().function_name ==
                std::optional<std::string>{"fail"},
            "array OOB did not identify the trapping function");

    (void)require_schedule_stable_failure(
        "string_oob",
        "fn fail() -> i64 {\n"
        "  \"x\"[4]\n"
        "}\n"
        "fail()\n",
        "string index out of bounds");

    (void)require_schedule_stable_failure(
        "map_missing_key",
        "fn fail() -> i64 {\n"
        "  let values: map<i64, i64> = map<i64, i64>();\n"
        "  values[4]\n"
        "}\n"
        "fail()\n",
        "map key not found");

    (void)require_schedule_stable_failure(
        "str_to_i64_grammar",
        "fn fail() -> i64 {\n"
        "  to_i64(\"01\")\n"
        "}\n"
        "fail()\n",
        "runtime trap at function 1 pc 1: invalid string for i64 conversion");

    (void)require_schedule_stable_failure(
        "substring_bounds",
        "fn fail() -> str {\n"
        "  \"abc\".sub(0, 4)\n"
        "}\n"
        "fail()\n",
        "string substring bounds out of range");

    const auto call_depth = require_schedule_stable_failure(
        "call_depth",
        "fn recurse(n: i64) -> i64 {\n"
        "  recurse(n + 1)\n"
        "}\n"
        "recurse(0)\n",
        "VM call depth limit exceeded", 6);
    require(call_depth.trace.frames.size() == 6,
            "call-depth trace did not preserve every admitted frame");

    const auto uncaught = require_schedule_stable_failure(
        "uncaught_exception",
        "variant Fault { Boom(i64) }\n"
        "fn deepest() -> i64 {\n"
        "  throw Fault.Boom(7);\n"
        "  0\n"
        "}\n"
        "fn middle() -> i64 {\n"
        "  deepest()\n"
        "}\n"
        "fn outer() -> i64 {\n"
        "  middle()\n"
        "}\n"
        "outer()\n",
        "uncaught exception Fault");
    require(uncaught.trace.kind ==
                lang::RuntimeFailureKind::UncaughtException &&
                uncaught.trace.exception_variant ==
                    std::optional<std::string>{"Fault"},
            "uncaught matrix case lost its failure kind or variant");

    const auto tail = require_schedule_stable_failure(
        "tail_called_trap",
        "fn fail() -> i64 {\n"
        "  to_i64(\"bad\")\n"
        "}\n"
        "fn bounce() -> i64 {\n"
        "  return tail fail();\n"
        "  0\n"
        "}\n"
        "bounce()\n",
        "runtime trap at function 1 pc 1: invalid string for i64 conversion");
    require(tail.trace.frames.size() == 2,
            "tail-called trap retained a reused history frame");
    require(tail.trace.frames[0].function_name ==
                std::optional<std::string>{"fail"} &&
                tail.trace.frames[1].function_name ==
                    std::optional<std::string>{"<entry>"},
            "tail-called trace did not contain current callee plus entry");
    for (const auto& frame : tail.trace.frames) {
        require(frame.function_name !=
                    std::optional<std::string>{"bounce"},
                "tail-called trace retained the reused caller");
    }

    const auto generic = require_schedule_stable_failure(
        "generic_instantiation",
        "fn generic_fail<T>(value: T) -> i64 {\n"
        "  to_i64(\"bad\")\n"
        "}\n"
        "generic_fail<i64>(7)\n",
        "runtime trap at function 1 pc 1: invalid string for i64 conversion");
    require(generic.trace.frames.front().function_name.has_value() &&
                generic.trace.frames.front().function_name->starts_with(
                    "generic_fail$mono$"),
            "generic trace omitted its monomorphized name");
    require_position(generic.trace.frames.front().source_position, 2, 3,
                     "generic template trap");

    const auto for_in = require_schedule_stable_failure(
        "for_in_growth",
        "let values: map<i64, i64> = map<i64, i64>();\n"
        "values[1] = 10;\n"
        "for key, value in values {\n"
        "  values[2] = value;\n"
        "}\n"
        "0\n",
        "map entry index out of bounds");
    require(for_in.trace.frames.size() == 1,
            "top-level for-in trap invented another frame");
    require_position(for_in.trace.frames.front().source_position, 3, 1,
                     "for-in mutation guard");
}

void shared_fuzz_outcome_compares_trace_as_a_third_oracle() {
    auto compiled = compile_or_throw(
        "fn fail() -> i64 {\n"
        "  to_i64(\"bad\")\n"
        "}\n"
        "fail()\n");
    const auto schedules = fuzz::schedules();
    const auto baseline =
        fuzz::execute_once(*compiled.verified_module, schedules.front());
    const auto observed =
        fuzz::execute_once(*compiled.verified_module, schedules[1]);
    require(!baseline.ok && !observed.ok,
            "shared deliberate-trap executions unexpectedly succeeded");
    require(baseline.trace.has_value() && observed.trace.has_value(),
            "shared fuzz outcome omitted a trapping trace");
    require(fuzz::same_observables(baseline, observed),
            "shared fuzz oracle rejected equal traces");

    auto changed_pc = observed;
    ++changed_pc.trace->frames.front().pc;
    require(!fuzz::same_observables(baseline, changed_pc),
            "shared fuzz oracle ignored a changed trace pc");

    auto changed_name = observed;
    changed_name.trace->frames.front().function_name = "different";
    require(!fuzz::same_observables(baseline, changed_name),
            "shared fuzz oracle ignored a changed function name");

    auto changed_position = observed;
    require(changed_position.trace->frames.front().source_position.has_value(),
            "shared fuzz setup omitted its source position");
    ++changed_position.trace->frames.front().source_position->column;
    require(!fuzz::same_observables(baseline, changed_position),
            "shared fuzz oracle ignored a changed source position");
}

struct TestCase {
    const char* name;
    const char* proves;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"hand_built_module_trace_falls_back_to_index_and_pc",
         "modules without debug tables still expose stable frame coordinates",
         hand_built_module_trace_falls_back_to_index_and_pc},
        {"frontend_debug_tables_are_complete_and_deterministic",
         "identical source compiles produce complete identical debug tables",
         frontend_debug_tables_are_complete_and_deterministic},
        {"nested_trace_normalizes_active_and_suspended_pcs",
         "active pc and suspended call-site pcs are reported innermost-first",
         nested_trace_normalizes_active_and_suspended_pcs},
        {"successful_execution_clears_a_prior_trace",
         "the failure side channel is execution-local rather than stale state",
         successful_execution_clears_a_prior_trace},
        {"caught_exception_leaves_no_terminal_trace",
         "handled language exceptions do not masquerade as terminal failures",
         caught_exception_leaves_no_terminal_trace},
        {"uncaught_exception_preserves_pre_unwind_frames_and_variant",
         "uncaught typed exceptions snapshot all frames and the variant name",
         uncaught_exception_preserves_pre_unwind_frames_and_variant},
        {"uncaught_exception_preserves_a_suspended_closure_frame",
         "CallClosure frames survive pre-unwind capture under moving stress",
         uncaught_exception_preserves_a_suspended_closure_frame},
        {"trapping_matrix_has_identical_traces_across_all_schedules",
         "ten trap families produce the same trace under all 15 schedules",
         trapping_matrix_has_identical_traces_across_all_schedules},
        {"shared_fuzz_outcome_compares_trace_as_a_third_oracle",
         "the shared harness detects pc, name, and source trace divergence",
         shared_fuzz_outcome_compares_trace_as_a_third_oracle},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << " | proves: "
                      << test.proves << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << " | proves: "
                      << test.proves << "\n"
                      << error.what() << "\n";
        }
    }
    if (failures != 0) {
        std::cerr << failures
                  << " iteration-45 runtime diagnostic test(s) failed\n";
        return 1;
    }
    return 0;
}
