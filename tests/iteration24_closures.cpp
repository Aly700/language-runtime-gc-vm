#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr lang::ObjectId kSlotMask = 0xFFFF'FFFFull;

std::uint32_t slot_of(lang::ObjectId id) {
    return static_cast<std::uint32_t>(id & kSlotMask);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_throws(Fn&& fn, const std::string& message) {
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

struct VectorRoots final : lang::gc::RootProvider {
    std::vector<lang::Value> roots;

    void trace_roots(lang::gc::RootVisitor& visitor) override {
        for (auto& root : roots) {
            visitor.visit(root);
        }
    }
};

lang::SignatureValue fn_type(std::vector<lang::SignatureValue> parameters,
                             lang::SignatureValue result) {
    return lang::function_signature(std::move(parameters), std::move(result));
}

lang::FunctionSignature make_function_signature(
    std::vector<lang::SignatureValue> parameters,
    lang::SignatureValue result) {
    lang::FunctionSignature signature;
    signature.parameter_types = parameters;
    for (const auto& parameter : parameters) {
        signature.parameters.push_back(parameter.kind);
    }
    signature.return_type = result.kind;
    signature.return_type_detail = std::move(result);
    return signature;
}

lang::ClosureLayout closure_layout(
    std::size_t function_index,
    std::vector<lang::SignatureValue> parameters,
    lang::SignatureValue result,
    std::vector<lang::SignatureValue> captures) {
    lang::ClosureLayout layout;
    layout.function_index = function_index;
    layout.function_type = fn_type(std::move(parameters), std::move(result));
    layout.capture_types = std::move(captures);
    for (const auto& capture : layout.capture_types) {
        layout.capture_map.push_back(lang::signature_value_is_reference(capture));
    }
    return layout;
}

void interleaved_capture_map_never_traces_or_forwards_scalar_object_id_bits() {
    lang::gc::Heap heap;
    const auto dead_a = heap.allocate_pair(lang::Value::int64(-1),
                                           lang::Value::int64(-2));
    const auto dead_b = heap.allocate_pair(lang::Value::int64(-3),
                                           lang::Value::int64(-4));
    const auto live_a = heap.allocate_pair(lang::Value::int64(10),
                                           lang::Value::int64(11));
    const auto live_b = heap.allocate_pair(lang::Value::int64(20),
                                           lang::Value::int64(21));

    const std::vector<bool> capture_map{true, false, true, false};
    const auto closure = heap.allocate_closure(
        7, 3,
        {lang::Value::object(live_a),
         lang::Value::int64(static_cast<std::int64_t>(dead_a)),
         lang::Value::object(live_b),
         lang::Value::int64(static_cast<std::int64_t>(dead_b))},
        capture_map);

    VectorRoots roots;
    roots.roots = {lang::Value::object(closure)};
    heap.collect(roots);

    const auto moved_closure = roots.roots.at(0).as_object();
    require(heap.live_count() == 3,
            "scalar closure captures kept dead ObjectIds live");
    require(heap.closure_capture(moved_closure, 1).as_i64() ==
                static_cast<std::int64_t>(dead_a) &&
                heap.closure_capture(moved_closure, 3).as_i64() ==
                static_cast<std::int64_t>(dead_b),
            "scalar closure captures were rewritten during compaction");

    const auto moved_a = heap.closure_capture(moved_closure, 0).as_object();
    const auto moved_b = heap.closure_capture(moved_closure, 2).as_object();
    require(heap.left(moved_a).as_i64() == 10 &&
                heap.left(moved_b).as_i64() == 20,
            "mapped reference captures were not retained and forwarded");
    require(moved_a != live_a && moved_b != live_b,
            "test setup did not relocate both mapped captures");
    require_throws([&] { (void)heap.object(dead_a); },
                   "dead ObjectId in scalar capture remained valid");
    require_throws([&] { (void)heap.object(dead_b); },
                   "second dead ObjectId in scalar capture remained valid");
}

void closure_storage_width_and_metadata_are_descriptor_owned() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_closure(
        0, 1,
        {lang::Value::int64(1), lang::Value::boolean(true),
         lang::Value::int64(3), lang::Value::int64(4)},
        {false, false, false, false});
    const auto live = heap.allocate_closure(
        9, 12, {lang::Value::int64(42)}, {false});
    const auto tail = heap.allocate_pair(lang::Value::int64(5),
                                         lang::Value::int64(6));

    VectorRoots roots;
    roots.roots = {lang::Value::object(live), lang::Value::object(tail)};
    heap.collect(roots);

    const auto moved_live = roots.roots.at(0).as_object();
    const auto moved_tail = roots.roots.at(1).as_object();
    require(slot_of(moved_live) == 0,
            "live closure did not slide over dead closure storage");
    require(slot_of(moved_tail) == 2,
            "compaction did not advance by function-index plus capture storage width");
    require(heap.closure_layout_index(moved_live) == 9 &&
                heap.closure_function_index(moved_live) == 12 &&
                heap.closure_capture_count(moved_live) == 1 &&
                heap.closure_capture(moved_live, 0).as_i64() == 42,
            "closure descriptor metadata or immutable capture changed");
    require_throws(
        [&] {
            (void)heap.allocate_closure(
                0, 0, {lang::Value::int64(1)}, {true});
        },
        "heap accepted capture-map/runtime-tag disagreement");
    (void)dead;
}

lang::Module moving_closure_call_module() {
    lang::Module module;
    module.entry_function = 0;
    module.closure_layouts.push_back(closure_layout(
        1, {}, lang::signature_value(lang::ValueKind::Int64),
        {lang::pair_signature(lang::signature_value(lang::ValueKind::Int64),
                              lang::signature_value(lang::ValueKind::Int64))}));

    lang::Function entry;
    entry.signature = make_function_signature(
        {}, lang::signature_value(lang::ValueKind::Int64));
    entry.local_count = 2;
    entry.code = {
        {lang::OpCode::ConstantI64, -1},
        {lang::OpCode::ConstantI64, -2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::Nil, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 41},
        {lang::OpCode::ConstantI64, 99},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::AllocClosure, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::CallClosure, 0},
        {lang::OpCode::Return, 0},
    };

    lang::Function body;
    body.signature = make_function_signature(
        {}, lang::signature_value(lang::ValueKind::Int64));
    body.closure_layout = 0;
    body.code = {
        {lang::OpCode::LoadCapture, 0},
        {lang::OpCode::GetLeft, 0},
        {lang::OpCode::Return, 0},
    };
    module.functions = {std::move(entry), std::move(body)};
    return module;
}

void closure_call_after_compaction_uses_forwarded_closure_and_capture() {
    auto module = moving_closure_call_module();
    const auto report = lang::verify_module_with_diagnostics(module);
    require(report.module.has_value(),
            "verifier rejected valid moving closure module");
    require(report.module->verification().functions.at(0).stack_maps.at(13)
                .object_slots == std::vector<bool>{true},
            "CallClosure callee was not a precise stack-map object root");

    lang::VM vm;
    const auto result = vm.execute(*report.module);
    require(result.as_i64() == 41,
            "LoadCapture returned the wrong moved capture after closure call");
    require(vm.metrics().heap.objects_moved >= 2,
            "explicit compaction did not relocate both closure and captured object");
}

void nil_capture_is_opaque_and_executes_after_verification() {
    lang::Module module;
    module.entry_function = 0;
    module.closure_layouts.push_back(closure_layout(
        1, {}, lang::signature_value(lang::ValueKind::Nil),
        {lang::signature_value(lang::ValueKind::Nil)}));

    lang::Function entry;
    entry.signature = make_function_signature(
        {}, lang::signature_value(lang::ValueKind::Nil));
    entry.code = {{lang::OpCode::Nil, 0},
                  {lang::OpCode::AllocClosure, 0},
                  {lang::OpCode::CallClosure, 0},
                  {lang::OpCode::Return, 0}};

    lang::Function body;
    body.signature = make_function_signature(
        {}, lang::signature_value(lang::ValueKind::Nil));
    body.closure_layout = 0;
    body.code = {{lang::OpCode::LoadCapture, 0},
                 {lang::OpCode::Return, 0}};
    module.functions = {std::move(entry), std::move(body)};

    auto report = lang::verify_module_with_diagnostics(std::move(module));
    require(report.module.has_value(),
            "verifier rejected a statically nil closure capture" +
                (report.diagnostics.empty()
                     ? std::string{}
                     : ": " + lang::format_verifier_diagnostic(
                                   report.diagnostics.front())));
    lang::VM vm;
    const auto result = vm.execute(*report.module);
    require(result.tag() == lang::Value::Tag::Nil,
            "nil closure capture did not round-trip opaquely");
}

lang::VerifierReason first_reason(const lang::Module& module) {
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value() && !report.diagnostics.empty(),
            "malformed closure module had no verifier diagnostic");
    return report.diagnostics.front().reason;
}

lang::Module one_argument_closure_module(bool include_argument,
                                         bool argument_is_bool) {
    lang::Module module;
    module.entry_function = 0;
    module.closure_layouts.push_back(closure_layout(
        1, {lang::signature_value(lang::ValueKind::Int64)},
        lang::signature_value(lang::ValueKind::Int64), {}));

    lang::Function entry;
    entry.signature = make_function_signature(
        {}, lang::signature_value(lang::ValueKind::Int64));
    if (include_argument) {
        if (argument_is_bool) {
            entry.code.push_back({lang::OpCode::ConstantI64, 0});
            entry.code.push_back({lang::OpCode::ConstantI64, 1});
            entry.code.push_back({lang::OpCode::LessI64, 0});
        } else {
            entry.code.push_back({lang::OpCode::ConstantI64, 7});
        }
    }
    entry.code.push_back({lang::OpCode::AllocClosure, 0});
    entry.code.push_back({lang::OpCode::CallClosure, 0});
    entry.code.push_back({lang::OpCode::Return, 0});

    lang::Function body;
    body.signature = make_function_signature(
        {lang::signature_value(lang::ValueKind::Int64)},
        lang::signature_value(lang::ValueKind::Int64));
    body.local_count = 1;
    body.closure_layout = 0;
    body.code = {{lang::OpCode::LoadLocal, 0},
                 {lang::OpCode::Return, 0}};
    module.functions = {std::move(entry), std::move(body)};
    return module;
}

void verifier_rejects_each_new_closure_boundary_with_stable_reason() {
    {
        lang::Function function;
        function.signature = make_function_signature(
            {lang::signature_value(lang::ValueKind::Function)},
            lang::signature_value(lang::ValueKind::Int64));
        function.local_count = 1;
        function.code = {{lang::OpCode::ConstantI64, 0},
                         {lang::OpCode::Return, 0}};
        require(first_reason(lang::Module{{std::move(function)}}) ==
                    lang::VerifierReason::SignatureShapeMismatch,
                "non-structural Function signature metadata was accepted");
    }
    {
        auto module = moving_closure_call_module();
        module.functions[0].code[9].operand = 4;
        require(first_reason(module) == lang::VerifierReason::BadClosureLayoutIndex,
                "bad AllocClosure layout used wrong reason");
    }
    {
        auto module = moving_closure_call_module();
        module.functions[1].closure_layout.reset();
        require(first_reason(module) == lang::VerifierReason::BadClosureLayoutIndex,
                "layout target without matching closure-body identity used wrong reason");
    }
    {
        auto module = moving_closure_call_module();
        module.functions[0].code.erase(module.functions[0].code.begin() + 6,
                                       module.functions[0].code.begin() + 9);
        require(first_reason(module) == lang::VerifierReason::BadClosureCaptureArity,
                "missing capture used wrong reason");
    }
    {
        auto module = moving_closure_call_module();
        module.closure_layouts[0].capture_types[0] =
            lang::signature_value(lang::ValueKind::Int64);
        module.closure_layouts[0].capture_map[0] = false;
        require(first_reason(module) == lang::VerifierReason::BadClosureCaptureType,
                "wrong capture type used wrong reason");
    }
    {
        auto module = moving_closure_call_module();
        module.closure_layouts[0].capture_types[0] =
            lang::signature_value(lang::ValueKind::Function);
        module.closure_layouts[0].capture_map[0] = true;
        require(first_reason(module) ==
                    lang::VerifierReason::BadClosureCaptureType,
                "non-structural Function capture metadata was accepted");
    }
    {
        auto module = moving_closure_call_module();
        module.functions[0].code[12] = {lang::OpCode::ConstantI64, 1};
        require(first_reason(module) == lang::VerifierReason::CallClosureOnNonFunction,
                "CallClosure on i64 used wrong reason");
    }
    require(first_reason(one_argument_closure_module(false, false)) ==
                lang::VerifierReason::BadClosureCallArity,
            "CallClosure argument underflow used wrong reason");
    require(first_reason(one_argument_closure_module(true, true)) ==
                lang::VerifierReason::BadClosureCallArgKind,
            "CallClosure wrong argument type used wrong reason");
    {
        auto module = moving_closure_call_module();
        module.functions[1].code[0].operand = 8;
        require(first_reason(module) == lang::VerifierReason::LoadCaptureOutOfRange,
                "bad capture index used wrong reason");
    }
    {
        auto module = moving_closure_call_module();
        module.functions[0].code[0] = {lang::OpCode::LoadCapture, 0};
        require(first_reason(module) ==
                    lang::VerifierReason::LoadCaptureOutsideClosureBody,
                "LoadCapture outside closure body used wrong reason");
    }
    {
        auto module = moving_closure_call_module();
        module.functions[0].local_count = 0;
        module.functions[0].code = {{lang::OpCode::Call, 1},
                                    {lang::OpCode::Return, 0}};
        require(first_reason(module) == lang::VerifierReason::BadCallTarget,
                "direct Call reached a capture-bearing closure body");
    }
    {
        auto module = moving_closure_call_module();
        module.entry_function = 1;
        require(first_reason(module) == lang::VerifierReason::BadCallTarget,
                "module entry reached a capture-bearing closure body without a closure frame");
        require(!lang::verify_module(std::move(module)).has_value(),
                "capture-bearing entry produced an executable VerifiedModule");
    }
}

void verifier_rejects_incompatible_function_refarray_elements() {
    lang::Module module;
    module.entry_function = 0;
    module.closure_layouts.push_back(closure_layout(
        1, {lang::signature_value(lang::ValueKind::Int64)},
        lang::signature_value(lang::ValueKind::Int64), {}));
    module.closure_layouts.push_back(closure_layout(
        2, {lang::signature_value(lang::ValueKind::Bool)},
        lang::signature_value(lang::ValueKind::Int64), {}));

    lang::Function entry;
    entry.signature = make_function_signature(
        {}, lang::signature_value(lang::ValueKind::Int64));
    entry.local_count = 1;
    entry.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::AllocClosure, 0},
        {lang::OpCode::AllocRefArray, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocClosure, 1},
        {lang::OpCode::RefArraySet, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::Return, 0},
    };

    lang::Function i64_body;
    i64_body.signature = make_function_signature(
        {lang::signature_value(lang::ValueKind::Int64)},
        lang::signature_value(lang::ValueKind::Int64));
    i64_body.local_count = 1;
    i64_body.closure_layout = 0;
    i64_body.code = {{lang::OpCode::LoadLocal, 0},
                     {lang::OpCode::Return, 0}};

    lang::Function bool_body;
    bool_body.signature = make_function_signature(
        {lang::signature_value(lang::ValueKind::Bool)},
        lang::signature_value(lang::ValueKind::Int64));
    bool_body.local_count = 1;
    bool_body.closure_layout = 1;
    bool_body.code = {{lang::OpCode::ConstantI64, 0},
                      {lang::OpCode::Return, 0}};

    module.functions = {std::move(entry), std::move(i64_body),
                        std::move(bool_body)};
    require(first_reason(module) == lang::VerifierReason::BadArrayOperation,
            "RefArray facts erased incompatible structural function types");
}

void call_closure_uses_deterministic_frame_depth_trap() {
    auto module = moving_closure_call_module();
    auto verified = lang::verify_module(std::move(module));
    require(verified.has_value(),
            "call-depth closure module failed verification");
    lang::VM vm;
    vm.set_max_call_depth(1);
    try {
        (void)vm.execute(*verified);
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "VM call depth limit exceeded",
                "CallClosure used a non-deterministic call-depth error");
        return;
    }
    throw std::runtime_error(
        "CallClosure bypassed the shared deterministic call-depth trap");
}

void promotion_path_records_old_closure_to_young_capture() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(-1),
                                         lang::Value::int64(-2));
    const auto young = heap.allocate_pair(lang::Value::int64(70),
                                          lang::Value::int64(71));
    auto closure = heap.allocate_closure(
        0, 0, {lang::Value::object(young)}, {true});
    VectorRoots roots;
    roots.roots = {lang::Value::object(closure)};

    heap.TEST_ONLY_promote_object_through_collector_path(closure);
    heap.TEST_ONLY_validate_gc_invariants();
    require(heap.TEST_ONLY_is_old_object(closure),
            "test hook did not route closure through collector promotion");
    require(heap.TEST_ONLY_is_young_object(young),
            "test setup did not preserve a young captured object");
    require(heap.TEST_ONLY_remembered_set_size() == 1,
            "promotion-created closure edge was not remembered");
    require(heap.metrics().write_barrier_hits == 0,
            "collector-internal promotion edge counted as a mutator barrier");
    const auto validations_before = heap.TEST_ONLY_validation_count();

    heap.collect_minor(roots);
    closure = roots.roots.at(0).as_object();
    const auto moved_capture = heap.closure_capture(closure, 0).as_object();
    require(slot_of(moved_capture) == slot_of(dead),
            "minor collection did not retain/forward promotion-created young capture");
    require(heap.left(moved_capture).as_i64() == 70,
            "promotion-created remembered edge retained the wrong object");
    require(heap.TEST_ONLY_remembered_set_size() == 0,
            "exact remembered-set pruning retained an old-to-old closure edge");
    require(heap.TEST_ONLY_validation_count() == validations_before + 1,
            "remembered-set validation did not run at the minor collection boundary");
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::string message = "source failed to compile:\n" + source + "\n";
        for (const auto& diagnostic : compiled.diagnostics) {
            message += std::to_string(diagnostic.position.line) + ":" +
                       std::to_string(diagnostic.position.column) + " " +
                       diagnostic.message + "\n";
        }
        throw std::runtime_error(message);
    }
    return compiled;
}

void require_diagnostic(const std::string& source, std::size_t line,
                        std::size_t column, const std::string& text) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "invalid closure source unexpectedly compiled: " + source);
    for (const auto& diagnostic : compiled.diagnostics) {
        if (diagnostic.position.line == line &&
            diagnostic.position.column == column &&
            diagnostic.message.find(text) != std::string::npos) {
            return;
        }
    }
    std::string observed;
    for (const auto& diagnostic : compiled.diagnostics) {
        observed += std::to_string(diagnostic.position.line) + ":" +
                    std::to_string(diagnostic.position.column) + " " +
                    diagnostic.message + "\n";
    }
    throw std::runtime_error("missing positioned diagnostic containing '" + text +
                             "' at " + std::to_string(line) + ":" +
                             std::to_string(column) + "\n" + observed);
}

void frontend_snapshot_semantics_survive_maximum_gc_stress() {
    const auto compiled = require_compiles(R"SRC(
let captured: i64 = 40;
let add: fn(i64) -> i64 = fn(x: i64) -> i64 {
  captured + x
};
captured = 100;
add(2)
)SRC");
    require(compiled.result_type == lang::frontend::Type::Int64,
            "closure call reported wrong public result type");

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    vm.set_gc_stress(stress);
    require(vm.execute(*compiled.verified_module).as_i64() == 42,
            "closure observed reassigned local instead of creation-time snapshot");
}

void frontend_returns_closures_and_captures_function_parameters() {
    const auto compiled = require_compiles(R"SRC(
fn make_adder(bias: i64) -> fn(i64) -> i64 {
  fn(value: i64) -> i64 {
    bias + value
  }
}

let add_ten: fn(i64) -> i64 = make_adder(10);
add_ten(5)
)SRC");
    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    vm.set_gc_stress(stress);
    require(vm.execute(*compiled.verified_module).as_i64() == 15,
            "returned closure did not retain its function-parameter snapshot");
}

void frontend_nested_lambdas_forward_outer_captures_by_value() {
    const auto compiled = require_compiles(R"SRC(
let base: i64 = 3;
let outer: fn(i64) -> fn(i64) -> i64 =
  fn(offset: i64) -> fn(i64) -> i64 {
    fn(value: i64) -> i64 {
      base + offset + value
    }
  };
base = 100;
let inner: fn(i64) -> i64 = outer(4);
inner(5)
)SRC");
    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    vm.set_gc_stress(stress);
    require(vm.execute(*compiled.verified_module).as_i64() == 12,
            "nested lambda did not forward outer capture and parameter snapshots");
}

void frontend_higher_order_pair_and_refarray_closures_run_under_stress() {
    const auto compiled = require_compiles(R"SRC(
fn apply(f: fn(i64) -> i64, value: i64) -> i64 {
  f(value)
}

fn increment(value: i64) -> i64 {
  value + 1
}

let bias: i64 = 7;
let biased: fn(i64) -> i64 = fn(value: i64) -> i64 {
  bias + value
};
let functions: [fn(i64) -> i64] = [biased, increment];
let holders: pair<fn(i64) -> i64, fn(i64) -> i64> =
  pair(functions[0], functions[1]);
apply(holders.left, 3) + apply(holders.right, 4)
)SRC");

    const auto& module = compiled.verified_module->module();
    bool saw_ref_array = false;
    bool saw_closure_call = false;
    for (const auto& function : module.functions) {
        for (const auto& instruction : function.code) {
            saw_ref_array = saw_ref_array ||
                            instruction.op == lang::OpCode::AllocRefArray;
            saw_closure_call = saw_closure_call ||
                               instruction.op == lang::OpCode::CallClosure;
        }
    }
    require(saw_ref_array && saw_closure_call,
            "function arrays/calls did not lower to RefArray and CallClosure");

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    stress.collect_minor_after_every_write_barrier = true;
    vm.set_gc_stress(stress);
    require(vm.execute(*compiled.verified_module).as_i64() == 15,
            "higher-order closure pair/array program returned wrong result");
}

void frontend_rejects_closure_misuse_with_positions() {
    require_diagnostic("1(2)", 1, 1, "cannot call non-function");
    require_diagnostic(
        "let f: fn(i64) -> i64 = fn(x: i64) -> i64 { x }; f()",
        1, 50, "expects 1 argument");
    require_diagnostic(
        "let f: fn(i64) -> i64 = fn(x: i64) -> i64 { x }; f(true)",
        1, 52, "expects i64 but got bool");
    require_diagnostic("fn(x: i64) -> i64 { true }", 1, 21,
                       "lambda returns bool but is declared i64");
    require_diagnostic(
        "let x: i64 = 1; let f: fn() -> i64 = fn() -> i64 { x = 2; x }; f()",
        1, 52, "cannot assign to immutable capture 'x'");
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"interleaved_capture_map_never_traces_or_forwards_scalar_object_id_bits",
         interleaved_capture_map_never_traces_or_forwards_scalar_object_id_bits},
        {"closure_storage_width_and_metadata_are_descriptor_owned",
         closure_storage_width_and_metadata_are_descriptor_owned},
        {"closure_call_after_compaction_uses_forwarded_closure_and_capture",
         closure_call_after_compaction_uses_forwarded_closure_and_capture},
        {"nil_capture_is_opaque_and_executes_after_verification",
         nil_capture_is_opaque_and_executes_after_verification},
        {"verifier_rejects_each_new_closure_boundary_with_stable_reason",
         verifier_rejects_each_new_closure_boundary_with_stable_reason},
        {"verifier_rejects_incompatible_function_refarray_elements",
         verifier_rejects_incompatible_function_refarray_elements},
        {"call_closure_uses_deterministic_frame_depth_trap",
         call_closure_uses_deterministic_frame_depth_trap},
        {"promotion_path_records_old_closure_to_young_capture",
         promotion_path_records_old_closure_to_young_capture},
        {"frontend_snapshot_semantics_survive_maximum_gc_stress",
         frontend_snapshot_semantics_survive_maximum_gc_stress},
        {"frontend_returns_closures_and_captures_function_parameters",
         frontend_returns_closures_and_captures_function_parameters},
        {"frontend_nested_lambdas_forward_outer_captures_by_value",
         frontend_nested_lambdas_forward_outer_captures_by_value},
        {"frontend_higher_order_pair_and_refarray_closures_run_under_stress",
         frontend_higher_order_pair_and_refarray_closures_run_under_stress},
        {"frontend_rejects_closure_misuse_with_positions",
         frontend_rejects_closure_misuse_with_positions},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << "\n" << e.what() << "\n";
        }
    }
    if (failures != 0) {
        std::cerr << failures << " iteration-24 closure test(s) failed\n";
        return 1;
    }
    return 0;
}
