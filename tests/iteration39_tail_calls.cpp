#include "fuzz_common.hpp"
#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr lang::ObjectId kSlotMask = 0xFFFF'FFFFull;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint32_t slot_of(lang::ObjectId id) {
    return static_cast<std::uint32_t>(id & kSlotMask);
}

void set_signature(lang::Function& function,
                   std::initializer_list<lang::ValueKind> parameters,
                   lang::ValueKind result) {
    function.signature.parameters.assign(parameters.begin(), parameters.end());
    function.signature.return_type = result;
}

lang::Function identity_function(lang::ValueKind kind) {
    lang::Function function;
    function.local_count = 1;
    set_signature(function, {kind}, kind);
    function.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
    };
    return function;
}

lang::Function constant_i64_function(std::int64_t value) {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Int64);
    function.code = {
        {lang::OpCode::ConstantI64, value},
        {lang::OpCode::Return, 0},
    };
    return function;
}

void require_reason(const lang::Module& module, lang::VerifierReason expected,
                    const std::string& context,
                    std::optional<std::size_t> expected_pc = std::nullopt) {
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value(), context + ": verifier unexpectedly accepted module");
    require(!report.diagnostics.empty(), context + ": verifier emitted no diagnostic");
    const auto& diagnostic = report.diagnostics.front();
    require(diagnostic.reason == expected,
            context + ": expected reason " + lang::verifier_reason_name(expected) +
                " but got " + lang::verifier_reason_name(diagnostic.reason) +
                "\n" + lang::format_verifier_diagnostic(diagnostic));
    if (expected_pc.has_value()) {
        require(diagnostic.pc == expected_pc,
                context + ": diagnostic pc mismatch\n" +
                    lang::format_verifier_diagnostic(diagnostic));
    }
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::ostringstream message;
        message << "expected source to compile\n" << source << "\n";
        for (const auto& diagnostic : compiled.diagnostics) {
            message << diagnostic.position.line << ":"
                    << diagnostic.position.column << " "
                    << diagnostic.message << "\n";
        }
        throw std::runtime_error(message.str());
    }
    require(compiled.verified_module.has_value(),
            "successful compile omitted VerifiedModule");
    return compiled;
}

void require_diagnostic(const std::string& source,
                        std::string_view expected_message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected source rejection\n" + source);
    require(!compiled.diagnostics.empty(),
            "source rejection omitted diagnostics\n" + source);
    const auto& diagnostic = compiled.diagnostics.front();
    require(diagnostic.message.find(expected_message) != std::string::npos,
            "expected diagnostic containing '" + std::string(expected_message) +
                "' but got '" + diagnostic.message + "'\n" + source);
    require(diagnostic.position.line != 0 && diagnostic.position.column != 0,
            "tail-call diagnostic omitted a source position");
}

lang::Module valid_tail_module() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    auto& caller = module.functions[0];
    set_signature(caller, {}, lang::ValueKind::Int64);
    caller.code = {
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::TailCall, 1},
    };
    module.functions[1] = identity_function(lang::ValueKind::Int64);
    return module;
}

void verifier_rejects_tail_call_matrix_with_stable_reasons() {
    {
        auto module = valid_tail_module();
        module.functions[0].code.back().operand = 99;
        require_reason(module, lang::VerifierReason::BadTailCallTarget,
                       "out-of-range TailCall target", 1);
    }
    {
        lang::Module module;
        module.entry_function = 0;
        module.functions.resize(2);
        set_signature(module.functions[0], {}, lang::ValueKind::Int64);
        module.functions[0].code = {{lang::OpCode::TailCall, 1}};

        auto& body = module.functions[1];
        set_signature(body, {}, lang::ValueKind::Int64);
        body.closure_layout = 0;
        body.code = {
            {lang::OpCode::LoadCapture, 0},
            {lang::OpCode::Return, 0},
        };
        module.closure_layouts.push_back(lang::ClosureLayout{
            1,
            lang::function_signature(
                {}, lang::signature_value(lang::ValueKind::Int64)),
            {lang::signature_value(lang::ValueKind::Int64)},
            {false},
        });
        require_reason(module, lang::VerifierReason::BadTailCallTarget,
                       "capture-bearing TailCall target", 0);
    }
    {
        auto module = valid_tail_module();
        module.functions[0].code = {{lang::OpCode::TailCall, 1}};
        require_reason(module, lang::VerifierReason::BadTailCallArity,
                       "TailCall missing argument", 0);
    }
    {
        auto module = valid_tail_module();
        module.functions[0].code.insert(
            module.functions[0].code.begin(),
            {lang::OpCode::ConstantI64, 7});
        require_reason(module, lang::VerifierReason::TailCallStackShapeMismatch,
                       "TailCall extra stack value", 2);
    }
    {
        auto module = valid_tail_module();
        module.functions[1] = identity_function(lang::ValueKind::Bool);
        set_signature(module.functions[0], {}, lang::ValueKind::Bool);
        require_reason(module, lang::VerifierReason::BadTailCallArgKind,
                       "TailCall wrong argument type", 1);
    }
    {
        auto module = valid_tail_module();
        set_signature(module.functions[0], {}, lang::ValueKind::Bool);
        require_reason(module,
                       lang::VerifierReason::TailCallReturnTypeMismatch,
                       "TailCall caller/callee return mismatch", 1);
    }
    {
        lang::Module module;
        module.entry_function = 0;
        module.functions.resize(2);
        module.variant_layouts.push_back(
            lang::VariantLayout{"Error",
                                {lang::VariantCaseLayout{"Boom", {}, {}}}});
        auto& caller = module.functions[0];
        set_signature(caller, {}, lang::ValueKind::Int64);
        caller.exception_handlers.push_back(
            lang::ExceptionHandler{0, 2, 4, 0});
        caller.code = {
            {lang::OpCode::TryBegin, 0},
            {lang::OpCode::TailCall, 1},
            {lang::OpCode::TryEnd, 0},
            {lang::OpCode::Jump, 5},
            {lang::OpCode::Throw, 0},
            {lang::OpCode::ConstantI64, 0},
            {lang::OpCode::Return, 0},
        };
        module.functions[1] = constant_i64_function(1);
        require_reason(module, lang::VerifierReason::TailCallInTryRegion,
                       "TailCall in active try region", 1);
    }
    {
        auto module = valid_tail_module();
        module.functions[0].code.push_back(
            {lang::OpCode::ConstantI64, 0});
        module.functions[0].code.push_back({lang::OpCode::Return, 0});
        require_reason(module, lang::VerifierReason::UnreachableCode,
                       "instruction after TailCall", 2);
    }

    require(std::string(lang::verifier_reason_name(
                        lang::VerifierReason::TailCallInTryRegion)) ==
                "TailCallInTryRegion" &&
                std::string(lang::verifier_reason_name(
                            lang::VerifierReason::BadTailCallTarget)) ==
                    "BadTailCallTarget" &&
                std::string(lang::verifier_reason_name(
                            lang::VerifierReason::TailCallReturnTypeMismatch)) ==
                    "TailCallReturnTypeMismatch" &&
                std::string(lang::verifier_reason_name(
                            lang::VerifierReason::BadTailCallArity)) ==
                    "BadTailCallArity" &&
                std::string(lang::verifier_reason_name(
                            lang::VerifierReason::TailCallStackShapeMismatch)) ==
                    "TailCallStackShapeMismatch" &&
                std::string(lang::verifier_reason_name(
                            lang::VerifierReason::BadTailCallArgKind)) ==
                    "BadTailCallArgKind",
            "TailCall reason names are not stable");
}

lang::Module tail_map_module() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    auto& caller = module.functions[0];
    caller.local_count = 1;
    set_signature(caller, {}, lang::ValueKind::Object);
    caller.code = {
        {lang::OpCode::ConstantI64, 10},
        {lang::OpCode::ConstantI64, 20},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::TailCall, 1},
    };
    module.functions[1] = identity_function(lang::ValueKind::Object);
    return module;
}

void tail_call_stack_map_exposes_only_outgoing_arguments() {
    auto module = tail_map_module();
    const auto report = lang::verify_with_diagnostics(module);
    require(report.result.has_value(),
            "verifier rejected valid TailCall stack-map module");
    const auto& map = report.result->functions[0].stack_maps[5];
    require(map.object_slots == std::vector<bool>{true},
            "TailCall map did not mark the outgoing object argument");
    require(map.local_object_slots == std::vector<bool>{false},
            "TailCall map exposed a dying reference local");

    module.functions[0].stack_maps = report.result->functions[0].stack_maps;
    module.functions[0].stack_maps[5].local_object_slots[0] = true;
    require_reason(module, lang::VerifierReason::BadStackMap,
                   "TailCall supplied stale-local map", 5);
}

void valid_tail_call_executes_and_returns_callee_value() {
    auto verified = lang::verify_module(valid_tail_module());
    require(verified.has_value(), "valid TailCall module did not verify");
    lang::VM vm;
    vm.set_max_call_depth(1);
    const auto result = vm.execute(*verified);
    require(result.tag() == lang::Value::Tag::Int64 &&
                result.as_i64() == 42,
            "TailCall did not return the callee value");
}

lang::Module boundary_precision_module() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    auto& caller = module.functions[0];
    caller.local_count = 1;
    set_signature(caller, {}, lang::ValueKind::Object);
    caller.code = {
        {lang::OpCode::ConstantI64, 900},
        {lang::OpCode::ConstantI64, 901},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 41},
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::TailCall, 1},
    };
    module.functions[1] = identity_function(lang::ValueKind::Object);
    return module;
}

void run_boundary_precision_case(lang::gc::StressConfig stress,
                                 std::string_view schedule,
                                 bool require_incremental_compaction_oracle) {
    auto verified = lang::verify_module(boundary_precision_module());
    require(verified.has_value(),
            "boundary-precision TailCall module did not verify");

    lang::VM vm;
    vm.set_gc_stress(std::move(stress));
    const auto result = vm.execute(*verified);
    require(result.is_object(),
            std::string(schedule) + ": TailCall returned a scalar");
    require(slot_of(result.as_object()) == 0,
            std::string(schedule) +
                ": outgoing root did not move into the dying local's slot");
    require(vm.heap().left(result.as_object()).as_i64() == 41 &&
                vm.heap().right(result.as_object()).as_i64() == 42,
            std::string(schedule) +
                ": outgoing argument was stale or corrupted after movement");
    require(vm.heap().live_count() == 1,
            std::string(schedule) +
                ": dying-frame local survived the TailCall boundary");
    require(vm.metrics().heap.objects_moved > 0,
            std::string(schedule) +
                ": boundary test did not exercise object movement");
    if (require_incremental_compaction_oracle) {
        require(vm.metrics()
                        .heap
                        .incremental_compaction_differential_validations > 0,
                std::string(schedule) +
                    ": incremental compaction differential oracle did not run");
    }
    require(!vm.heap().incremental_marking_active() &&
                !vm.heap().incremental_compaction_active(),
            std::string(schedule) +
                ": VM returned with an incremental phase active");
}

void boundary_collection_traces_arguments_not_dying_locals() {
    {
        lang::gc::StressConfig stress;
        stress.collect_every_n_instructions = 7;
        run_boundary_precision_case(stress, "atomic_tail_boundary", false);
    }
    {
        lang::gc::StressConfig stress;
        stress.incremental_mark_step_budgets = {1};
        run_boundary_precision_case(stress, "incremental_mark_tail_boundary",
                                    false);
    }
    {
        lang::gc::StressConfig stress;
        stress.incremental_compact_step_budgets = {1};
        run_boundary_precision_case(
            stress, "incremental_compact_tail_boundary", true);
    }
}

lang::Module closure_tail_boundary_module() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(3);
    module.closure_layouts.push_back(lang::ClosureLayout{
        2,
        lang::function_signature(
            {}, lang::signature_value(lang::ValueKind::Object)),
        {lang::signature_value(lang::ValueKind::Object)},
        {true},
    });

    auto& entry = module.functions[0];
    entry.local_count = 1;
    set_signature(entry, {}, lang::ValueKind::Object);
    entry.code = {
        {lang::OpCode::ConstantI64, 900},
        {lang::OpCode::ConstantI64, 901},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::Nil, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 31},
        {lang::OpCode::ConstantI64, 11},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::AllocClosure, 0},
        {lang::OpCode::CallClosure, 0},
        {lang::OpCode::Return, 0},
    };

    module.functions[1] = identity_function(lang::ValueKind::Object);

    auto& closure_body = module.functions[2];
    set_signature(closure_body, {}, lang::ValueKind::Object);
    closure_body.closure_layout = 0;
    closure_body.code = {
        {lang::OpCode::LoadCapture, 0},
        {lang::OpCode::TailCall, 1},
    };
    return module;
}

void tail_boundary_clears_closure_without_disturbing_handle_roots() {
    auto verified = lang::verify_module(closure_tail_boundary_module());
    require(verified.has_value(),
            "closure TailCall boundary module did not verify");

    lang::VM vm;
    auto handle = vm.heap().make_handle(vm.heap().allocate_pair(
        lang::Value::int64(70), lang::Value::int64(71)));
    lang::gc::StressConfig stress;
    stress.collect_every_n_instructions = 12;
    vm.set_gc_stress(stress);

    const auto result = vm.execute(*verified);
    require(result.is_object(),
            "closure TailCall boundary returned a scalar");
    require(slot_of(result.as_object()) == 1,
            "captured outgoing argument did not move over the dying pair; slot=" +
                std::to_string(slot_of(result.as_object())));
    require(vm.heap().left(result.as_object()).as_i64() == 31 &&
                vm.heap().right(result.as_object()).as_i64() == 11,
            "captured outgoing argument was lost while clearing the closure root");
    require(vm.heap().left(handle.object()).as_i64() == 70 &&
                vm.heap().right(handle.object()).as_i64() == 71,
            "TailCall boundary collection failed to rewrite an embedder handle");
    require(vm.heap().live_count() == 2,
            "dying local or frame closure survived the TailCall boundary");
    require(vm.metrics().heap.objects_moved > 0,
            "closure TailCall boundary did not exercise movement");
}

void million_deep_self_recursion_reuses_one_frame() {
    const auto compiled = require_compiles(R"(
fn loop(n: i64, acc: i64) -> i64 {
  if n < 1 {
  } else {
    return tail loop(n + -1, acc + 1);
  }
  acc
}
loop(1000001, 0)
)");
    lang::VM vm;
    vm.set_max_call_depth(2);
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Int64 &&
                result.as_i64() == 1'000'001,
            "million-deep TailCall recursion returned the wrong accumulator");
}

void mutual_tail_recursion_is_constant_space() {
    const auto compiled = require_compiles(R"(
fn even(n: i64) -> bool {
  if n < 1 {
  } else {
    return tail odd(n + -1);
  }
  true
}
fn odd(n: i64) -> bool {
  if n < 1 {
  } else {
    return tail even(n + -1);
  }
  false
}
even(100001)
)");
    lang::VM vm;
    vm.set_max_call_depth(2);
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Bool && !result.as_bool(),
            "mutual TailCall recursion returned the wrong parity");

    std::size_t tail_calls = 0;
    for (const auto& function :
         compiled.verified_module->module().functions) {
        for (const auto& instruction : function.code) {
            tail_calls += instruction.op == lang::OpCode::TailCall ? 1 : 0;
        }
    }
    require(tail_calls == 2,
            "compiler did not emit one explicit TailCall per mutual function");
}

void ordinary_recursion_keeps_the_depth_trap() {
    const auto compiled = require_compiles(R"(
fn recurse(n: i64) -> i64 {
  let result: i64 = 0;
  if n < 1 {
    result = n;
  } else {
    result = recurse(n + -1);
  }
  result
}
recurse(100)
)");
    lang::VM vm;
    vm.set_max_call_depth(2);
    try {
        (void)vm.execute(*compiled.verified_module);
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find("call depth") !=
                    std::string::npos,
                "ordinary recursion trapped for the wrong reason");
        return;
    }
    throw std::runtime_error(
        "ordinary recursion bypassed the configured call-depth trap");
}

void frontend_rejects_invalid_tail_positions_and_targets() {
    require_diagnostic(R"(
fn id(n: i64) -> i64 { n }
return tail id(1);
0
)",
                       "only allowed inside a function");
    require_diagnostic(R"(
fn bad() -> i64 {
  return tail 1;
  0
}
bad()
)",
                       "named function");
    require_diagnostic(R"(
fn id(n: i64) -> i64 { n }
fn bad(n: i64) -> i64 {
  let callable: fn(i64) -> i64 = id;
  return tail callable(n);
  0
}
bad(1)
)",
                       "named function");
    require_diagnostic(R"(
fn flag() -> bool { true }
fn bad() -> i64 {
  return tail flag();
  0
}
bad()
)",
                       "returns bool");
    require_diagnostic(R"(
variant Error { Boom() }
fn id(n: i64) -> i64 { n }
fn bad() -> i64 {
  try {
    return tail id(1);
  } catch (error: Error) {
  }
  0
}
bad()
)",
                       "active try region");
}

void exception_from_tail_callee_reaches_outer_handler() {
    const auto compiled = require_compiles(R"(
variant Error { Boom(i64) }
fn fail() -> i64 {
  throw Error.Boom(42);
  0
}
fn relay() -> i64 {
  return tail fail();
  0
}
let answer: i64 = 0;
try {
  answer = relay();
} catch (error: Error) {
  match error {
    Boom(code) => {
      answer = code;
    }
  }
}
print(to_str(answer));
answer
)");

    std::vector<lang::gc::StressConfig> schedules;
    {
        lang::gc::StressConfig stress;
        stress.collect_every_n_instructions = 1;
        schedules.push_back(stress);
    }
    {
        lang::gc::StressConfig stress;
        stress.incremental_mark_step_budgets = {1};
        schedules.push_back(stress);
    }
    {
        lang::gc::StressConfig stress;
        stress.incremental_compact_step_budgets = {1};
        schedules.push_back(stress);
    }

    for (const auto& stress : schedules) {
        lang::VM vm;
        vm.set_gc_stress(stress);
        const auto result = vm.execute(*compiled.verified_module);
        require(result.tag() == lang::Value::Tag::Int64 &&
                    result.as_i64() == 42,
                "outer handler did not receive TailCall callee exception");
        require(std::string(vm.output().begin(), vm.output().end()) == "42\n",
                "TailCall exception path changed deterministic output");
        require(!vm.heap().incremental_marking_active() &&
                    !vm.heap().incremental_compaction_active(),
                "TailCall exception escaped with an active GC phase");
    }
}

void reference_arguments_survive_mutual_frame_reuse() {
    const auto compiled = require_compiles(R"(
fn hop_a(n: i64, keep: pair<i64, i64>) -> pair<i64, i64> {
  let stale: pair<i64, i64> = pair(n + 1000, n + 2000);
  if n < 1 {
  } else {
    return tail hop_b(n + -1, keep);
  }
  keep
}
fn hop_b(n: i64, keep: pair<i64, i64>) -> pair<i64, i64> {
  let stale: pair<i64, i64> = pair(n + 3000, n + 4000);
  if n < 1 {
  } else {
    return tail hop_a(n + -1, keep);
  }
  keep
}
let keep: pair<i64, i64> = pair(20, 22);
let result: pair<i64, i64> = hop_a(200, keep);
print(to_str(result.left + result.right));
result
)");

    const auto all = fuzz::schedules();
    const auto baseline =
        fuzz::execute_once(*compiled.verified_module, all.front());
    require(baseline.ok, "reference TailCall baseline trapped");
    for (const auto name : {"incremental_1", "incremental_compact_1",
                            "combined_mark_compact"}) {
        const auto& schedule = fuzz::find_schedule(all, name);
        const auto observed =
            fuzz::execute_once(*compiled.verified_module, schedule);
        require(observed.ok, std::string(name) + ": reference TailCall trapped");
        require(fuzz::same_observables(baseline, observed),
                std::string(name) +
                    ": reference TailCall graph/output changed across GC");
    }
}

} // namespace

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests{
        {"verifier_rejects_tail_call_matrix_with_stable_reasons",
         verifier_rejects_tail_call_matrix_with_stable_reasons},
        {"tail_call_stack_map_exposes_only_outgoing_arguments",
         tail_call_stack_map_exposes_only_outgoing_arguments},
        {"valid_tail_call_executes_and_returns_callee_value",
         valid_tail_call_executes_and_returns_callee_value},
        {"boundary_collection_traces_arguments_not_dying_locals",
         boundary_collection_traces_arguments_not_dying_locals},
        {"tail_boundary_clears_closure_without_disturbing_handle_roots",
         tail_boundary_clears_closure_without_disturbing_handle_roots},
        {"million_deep_self_recursion_reuses_one_frame",
         million_deep_self_recursion_reuses_one_frame},
        {"mutual_tail_recursion_is_constant_space",
         mutual_tail_recursion_is_constant_space},
        {"ordinary_recursion_keeps_the_depth_trap",
         ordinary_recursion_keeps_the_depth_trap},
        {"frontend_rejects_invalid_tail_positions_and_targets",
         frontend_rejects_invalid_tail_positions_and_targets},
        {"exception_from_tail_callee_reaches_outer_handler",
         exception_from_tail_callee_reaches_outer_handler},
        {"reference_arguments_survive_mutual_frame_reuse",
         reference_arguments_survive_mutual_frame_reuse},
    };

    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << "\n";
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << "\n";
            return 1;
        }
    }
    return 0;
}
