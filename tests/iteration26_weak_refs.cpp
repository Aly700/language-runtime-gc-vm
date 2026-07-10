#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
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

void require_stale(lang::gc::Heap& heap, lang::ObjectId id,
                   const std::string& message) {
    bool stale = false;
    try {
        (void)heap.object(id);
    } catch (const std::out_of_range&) {
        stale = true;
    }
    require(stale, message);
}

void weak_target_does_not_retain_referent_but_strong_field_does() {
    {
        lang::gc::Heap heap;
        const auto target = heap.allocate_pair(lang::Value::int64(10),
                                               lang::Value::int64(11));
        auto weak = heap.make_handle(
            heap.allocate_weak(lang::Value::object(target)));

        heap.collect();

        require(heap.weak_get(weak.object()).tag() == lang::Value::Tag::Nil,
                "major collection retained a weak-only referent");
        require(heap.live_count() == 1,
                "weak registry extended referent liveness");
        require_stale(heap, target,
                      "weak-only referent id remained valid after collection");
        heap.TEST_ONLY_validate_gc_invariants();
    }

    {
        lang::gc::Heap heap;
        const auto target = heap.allocate_pair(lang::Value::int64(20),
                                               lang::Value::int64(21));
        auto strong = heap.make_handle(heap.allocate_pair(
            lang::Value::object(target), lang::Value::nil()));

        heap.collect();

        const auto retained = heap.left(strong.object());
        require(retained.is_object() &&
                    heap.right(retained.as_object()).as_i64() == 21,
                "strong descriptor field failed to retain its referent");
        require(heap.live_count() == 2,
                "strong-edge contrast retained the wrong object count");
    }
}

void full_compaction_forwards_live_weak_target() {
    lang::gc::Heap heap;
    (void)heap.allocate_pair(lang::Value::int64(-1), lang::Value::int64(-2));
    auto target = heap.make_handle(heap.allocate_pair(lang::Value::int64(30),
                                                      lang::Value::int64(31)));
    auto weak = heap.make_handle(
        heap.allocate_weak(lang::Value::object(target.object())));
    const auto old_target = target.object();

    heap.collect();

    const auto observed = heap.weak_get(weak.object());
    require(observed.is_object(),
            "live target was cleared during full compaction");
    require(observed.as_object() == target.object(),
            "weak slot and strong root disagree on forwarded target id");
    require(observed.as_object() != old_target,
            "test setup did not move the weak target");
    require(heap.left(observed.as_object()).as_i64() == 30 &&
                heap.right(observed.as_object()).as_i64() == 31,
            "forwarded weak target payload changed");
    require_stale(heap, old_target,
                  "weak slot retained a stale pre-compaction target id");
    heap.TEST_ONLY_validate_gc_invariants();
}

void map_growth_relocation_forwards_weak_target() {
    lang::gc::Heap heap;
    auto target = heap.make_handle(heap.allocate_map(0, false, false));
    auto weak = heap.make_handle(
        heap.allocate_weak(lang::Value::object(target.object())));
    const auto old_target = target.object();

    // The WeakRef occupies the map's adjacent slot, so the first append must use
    // the deterministic out-of-collection relocation path.
    heap.map_set(target.object(), lang::Value::int64(1),
                 lang::Value::int64(2));

    require(target.object() != old_target,
            "test setup did not relocate the weak map target");
    const auto observed = heap.weak_get(weak.object());
    require(observed.is_object() && observed.as_object() == target.object(),
            "map-growth movement left a stale weak target");
    require(heap.map_get(observed.as_object(), lang::Value::int64(1)).as_i64() == 2,
            "relocated weak map target lost its entry");
    require_stale(heap, old_target,
                  "pre-relocation weak map target id remained valid");
    heap.TEST_ONLY_validate_gc_invariants();
}

void old_weak_ref_clears_dead_young_target_in_minor_collection() {
    lang::gc::Heap heap;
    const auto target = heap.allocate_pair(lang::Value::int64(40),
                                           lang::Value::int64(41));
    auto weak = heap.make_handle(
        heap.allocate_weak(lang::Value::object(target)));
    heap.TEST_ONLY_promote_object_through_collector_path(weak.object());
    require(heap.TEST_ONLY_is_old_object(weak.object()),
            "test setup did not create an old WeakRef owner");
    require(heap.TEST_ONLY_is_young_object(target),
            "test setup did not retain a young weak target");

    heap.collect_minor();

    require(heap.weak_get(weak.object()).tag() == lang::Value::Tag::Nil,
            "minor collection retained a young weak-only target");
    require_stale(heap, target,
                  "minor-dead weak target id remained valid");
    require(heap.TEST_ONLY_remembered_set_size() == 0,
            "weak edge entered the remembered set");
    heap.TEST_ONLY_validate_gc_invariants();
}

void old_weak_ref_forwards_strongly_rooted_young_target_in_minor_collection() {
    lang::gc::Heap heap;
    (void)heap.allocate_pair(lang::Value::int64(-3), lang::Value::int64(-4));
    auto target = heap.make_handle(heap.allocate_pair(lang::Value::int64(50),
                                                      lang::Value::int64(51)));
    auto weak = heap.make_handle(
        heap.allocate_weak(lang::Value::object(target.object())));
    heap.TEST_ONLY_promote_object_through_collector_path(weak.object());
    const auto old_target = target.object();

    heap.collect_minor();

    const auto observed = heap.weak_get(weak.object());
    require(observed.is_object() && observed.as_object() == target.object(),
            "minor collection did not forward a surviving weak target");
    require(heap.TEST_ONLY_is_old_object(observed.as_object()),
            "surviving young target was not promoted");
    require(heap.right(observed.as_object()).as_i64() == 51,
            "minor-forwarded target payload changed");
    require(observed.as_object() != old_target,
            "test setup did not move the young target during minor collection");
    heap.TEST_ONLY_validate_gc_invariants();
}

void cleared_weak_ref_never_resurrects_after_slot_reuse() {
    lang::gc::Heap heap;
    (void)heap.allocate_pair(lang::Value::int64(-5), lang::Value::int64(-6));
    const auto target = heap.allocate_pair(lang::Value::int64(60),
                                           lang::Value::int64(61));
    auto weak = heap.make_handle(
        heap.allocate_weak(lang::Value::object(target)));

    heap.collect();
    require(heap.weak_get(weak.object()).tag() == lang::Value::Tag::Nil,
            "initial collection did not clear weak target");

    const auto replacement = heap.allocate_pair(lang::Value::int64(70),
                                                lang::Value::int64(71));
    require(replacement != target,
            "generation-tagged slot reuse aliased the stale target id");
    require(heap.weak_get(weak.object()).tag() == lang::Value::Tag::Nil,
            "slot reuse resurrected a cleared weak reference");

    heap.collect();
    require(heap.weak_get(weak.object()).tag() == lang::Value::Tag::Nil,
            "later collection resurrected a cleared weak reference");
    heap.TEST_ONLY_validate_gc_invariants();
}

void weak_ref_to_weak_ref_keeps_only_strongly_rooted_weak_owner_alive() {
    lang::gc::Heap heap;
    const auto target = heap.allocate_pair(lang::Value::int64(80),
                                           lang::Value::int64(81));
    auto inner = heap.make_handle(
        heap.allocate_weak(lang::Value::object(target)));
    auto outer = heap.make_handle(
        heap.allocate_weak(lang::Value::object(inner.object())));

    heap.collect();

    require(heap.weak_get(inner.object()).tag() == lang::Value::Tag::Nil,
            "inner weak reference retained its weak-only pair target");
    const auto outer_target = heap.weak_get(outer.object());
    require(outer_target.is_object() && outer_target.as_object() == inner.object(),
            "outer weak reference did not forward its live WeakRef target");
    require(heap.TEST_ONLY_is_weak_ref(outer_target.as_object()),
            "WeakRef-to-WeakRef target changed object kind");
    require(heap.live_count() == 2,
            "WeakRef-to-WeakRef graph retained an unexpected object");
}

void weak_ref_owner_is_a_normal_strong_referent_in_every_container_kind() {
    const auto run = [](const std::string& kind, auto make_owner) {
        lang::gc::Heap heap;
        const auto target = heap.allocate_pair(lang::Value::int64(90),
                                               lang::Value::int64(91));
        const auto weak = heap.allocate_weak(lang::Value::object(target));
        auto owner = heap.make_handle(make_owner(heap, weak));

        heap.collect();

        const auto moved_weak = [&]() -> lang::ObjectId {
            if (kind == "pair") {
                return heap.left(owner.object()).as_object();
            }
            if (kind == "ref-array") {
                return heap.ref_array_get(owner.object(), 0).as_object();
            }
            if (kind == "map") {
                return heap.map_get(owner.object(), lang::Value::int64(1)).as_object();
            }
            return heap.closure_capture(owner.object(), 0).as_object();
        }();
        require(heap.TEST_ONLY_is_weak_ref(moved_weak),
                kind + " did not strongly retain its WeakRef object");
        require(heap.weak_get(moved_weak).tag() == lang::Value::Tag::Nil,
                kind + " WeakRef target unexpectedly retained its referent");
        heap.TEST_ONLY_validate_gc_invariants();
    };

    run("pair", [](lang::gc::Heap& heap, lang::ObjectId weak) {
        return heap.allocate_pair(lang::Value::object(weak), lang::Value::nil());
    });
    run("ref-array", [](lang::gc::Heap& heap, lang::ObjectId weak) {
        return heap.allocate_ref_array(1, lang::Value::object(weak));
    });
    run("map", [](lang::gc::Heap& heap, lang::ObjectId weak) {
        const auto map = heap.allocate_map(0, false, true);
        heap.map_set(map, lang::Value::int64(1), lang::Value::object(weak));
        return map;
    });
    run("closure", [](lang::gc::Heap& heap, lang::ObjectId weak) {
        return heap.allocate_closure(0, 0, {lang::Value::object(weak)}, {true});
    });
}

lang::Module module_with_entry(lang::Function entry) {
    lang::Module module;
    module.entry_function = 0;
    module.functions.push_back(std::move(entry));
    return module;
}

lang::VerifierReason first_rejection_reason(const lang::Module& module) {
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value(), "expected verifier rejection");
    require(!report.diagnostics.empty(), "verifier rejection lacked a diagnostic");
    return report.diagnostics.front().reason;
}

void weak_opcodes_preserve_target_shape_and_is_nil_refinement() {
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.signature.return_type_detail =
        lang::signature_value(lang::ValueKind::Int64);
    entry.local_count = 3;
    entry.code = {
        {lang::OpCode::ConstantI64, 7},
        {lang::OpCode::ConstantI64, 8},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::AllocWeak, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::WeakGet, 0},
        {lang::OpCode::StoreLocal, 2},
        {lang::OpCode::LoadLocal, 2},
        {lang::OpCode::IsNil, 0},
        {lang::OpCode::JumpIfFalse, 16},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::Return, 0},
        {lang::OpCode::LoadLocal, 2},
        {lang::OpCode::GetLeft, 0},
        {lang::OpCode::Return, 0},
    };

    auto report = lang::verify_module_with_diagnostics(module_with_entry(entry));
    require(report.module.has_value(),
            "verifier rejected guarded WeakGet: " +
                (report.diagnostics.empty()
                     ? std::string("<no diagnostic>")
                     : lang::format_verifier_diagnostic(
                           report.diagnostics.front())));
    const auto& maps = report.module->verification().functions[0].stack_maps;
    require(maps[6].object_slots == std::vector<bool>{true},
            "AllocWeak result stack map is not a precise root");
    require(maps[10].object_slots == std::vector<bool>{true},
            "WeakGet maybe-target stack map is not a nil-able root");

    lang::VM vm;
    const auto result = vm.execute(*report.module);
    require(result.tag() == lang::Value::Tag::Int64 && result.as_i64() == 7,
            "guarded WeakGet lost the target's pair-field shape or payload");
}

void weak_get_observes_clearing_in_vm() {
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Bool;
    entry.signature.return_type_detail =
        lang::signature_value(lang::ValueKind::Bool);
    entry.local_count = 2;
    entry.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::AllocWeak, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::ConstantI64, 99},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::WeakGet, 0},
        {lang::OpCode::IsNil, 0},
        {lang::OpCode::Return, 0},
    };
    auto report = lang::verify_module_with_diagnostics(module_with_entry(entry));
    require(report.module.has_value(),
            "verifier rejected observable weak clearing");
    lang::VM vm;
    const auto result = vm.execute(*report.module);
    require(result.tag() == lang::Value::Tag::Bool && result.as_bool(),
            "WeakGet did not return nil after its target was collected");
}

void verifier_reports_stable_weak_reason_codes() {
    {
        lang::Function entry;
        entry.signature.return_type = lang::ValueKind::Int64;
        entry.code = {{lang::OpCode::ConstantI64, 1},
                      {lang::OpCode::AllocWeak, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(std::move(entry))) ==
                    lang::VerifierReason::BadWeakTargetType,
                "AllocWeak on scalar used the wrong verifier reason");
    }
    {
        lang::Function entry;
        entry.signature.return_type = lang::ValueKind::Int64;
        entry.code = {{lang::OpCode::ConstantI64, 1},
                      {lang::OpCode::ConstantI64, 2},
                      {lang::OpCode::AllocPair, 0},
                      {lang::OpCode::WeakGet, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(std::move(entry))) ==
                    lang::VerifierReason::WeakOperationOnNonWeak,
                "WeakGet on non-weak used the wrong verifier reason");
    }
    {
        lang::Function entry;
        entry.signature.return_type = lang::ValueKind::Int64;
        entry.code = {{lang::OpCode::ConstantI64, 1},
                      {lang::OpCode::ConstantI64, 2},
                      {lang::OpCode::AllocPair, 0},
                      {lang::OpCode::AllocWeak, 0},
                      {lang::OpCode::WeakGet, 0},
                      {lang::OpCode::GetLeft, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(std::move(entry))) ==
                    lang::VerifierReason::WeakTargetMayBeNil,
                "unguarded WeakGet use did not report nil refinement requirement");
    }
    {
        lang::Function entry;
        entry.signature.parameters = {lang::ValueKind::Weak};
        entry.signature.parameter_types = {lang::weak_signature(
            lang::signature_value(lang::ValueKind::Int64))};
        entry.signature.return_type = lang::ValueKind::Weak;
        entry.signature.return_type_detail = lang::weak_signature(
            lang::signature_value(lang::ValueKind::Int64));
        entry.local_count = 1;
        entry.code = {{lang::OpCode::LoadLocal, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(std::move(entry))) ==
                    lang::VerifierReason::BadWeakTargetType,
                "weak<i64> signature used the wrong verifier reason");
    }
}

void maybe_weak_target_rejoins_after_is_nil_branch() {
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Bool;
    entry.signature.return_type_detail =
        lang::signature_value(lang::ValueKind::Bool);
    entry.local_count = 2;
    entry.code = {
        {lang::OpCode::PushStr, 0},
        {lang::OpCode::AllocWeak, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::WeakGet, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::IsNil, 0},
        {lang::OpCode::JumpIfFalse, 10},
        {lang::OpCode::Jump, 10},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::IsNil, 0},
        {lang::OpCode::Return, 0},
    };
    auto module = module_with_entry(std::move(entry));
    module.string_constants = {"target"};
    const auto report = lang::verify_module_with_diagnostics(std::move(module));
    require(report.module.has_value(),
            "maybe weak target became poison after is_nil branch merge: " +
                (report.diagnostics.empty()
                     ? std::string("<no diagnostic>")
                     : lang::format_verifier_diagnostic(
                           report.diagnostics.front())));
}

lang::frontend::CompileResult compile_source(const std::string& source) {
    return lang::frontend::compile_program(source);
}

bool diagnostic_contains(const lang::frontend::CompileResult& result,
                         const std::string& needle) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void frontend_weak_get_refines_and_observes_collection() {
    const std::string source =
        "let target: pair<i64, i64> = pair(7, 8);\n"
        "let w: weak<pair<i64, i64>> = weak(target);\n"
        "target = pair(90, 91);\n"
        "let got: pair<i64, i64> = w.get();\n"
        "let result: i64 = 0;\n"
        "if is_nil(got) {\n"
        "  result = 1;\n"
        "} else {\n"
        "  result = got.left;\n"
        "}\n"
        "result\n";
    auto compiled = compile_source(source);
    require(compiled.ok(),
            "frontend rejected valid weak source: " +
                (compiled.diagnostics.empty()
                     ? std::string("<no diagnostic>")
                     : compiled.diagnostics.front().message));

    std::size_t target_store_count = 0;
    std::size_t drop_boundary = 0;
    const auto& entry_code =
        compiled.verified_module->module().functions[0].code;
    for (std::size_t pc = 0; pc < entry_code.size(); ++pc) {
        if (entry_code[pc].op == lang::OpCode::StoreLocal &&
            entry_code[pc].operand == 0 && ++target_store_count == 2) {
            drop_boundary = pc + 1;
            break;
        }
    }
    require(drop_boundary != 0,
            "compiled weak source did not contain the target drop store");

    const std::array schedules{
        lang::gc::StressConfig{.collect_every_n_instructions = 1},
        lang::gc::StressConfig{
            .collect_minor_every_n_instructions = drop_boundary},
        lang::gc::StressConfig{.collect_before_every_allocation = true,
                               .collect_after_every_allocation = true,
                               .collect_every_n_instructions = 3,
                               .collect_minor_every_n_instructions = 2},
    };
    for (std::size_t i = 0; i < schedules.size(); ++i) {
        const auto& stress = schedules[i];
        lang::VM vm;
        vm.set_gc_stress(stress);
        const auto result = vm.execute(*compiled.verified_module);
        require(result.tag() == lang::Value::Tag::Int64 &&
                    result.as_i64() == 1,
                "language WeakGet did not observe schedule-driven clearing for schedule " +
                    std::to_string(i) + ", result=" +
                    (result.tag() == lang::Value::Tag::Int64
                         ? std::to_string(result.as_i64())
                         : std::string("non-i64")));
        vm.heap().TEST_ONLY_validate_gc_invariants();
    }
}

void frontend_weak_target_survives_when_strongly_reachable() {
    const std::string source =
        "let target: pair<i64, i64> = pair(17, 18);\n"
        "let w: weak<pair<i64, i64>> = weak(target);\n"
        "let got: pair<i64, i64> = w.get();\n"
        "let result: i64 = 0;\n"
        "if is_nil(got) {\n"
        "  result = -1;\n"
        "} else {\n"
        "  result = got.right;\n"
        "}\n"
        "target = pair(result, 0);\n"
        "result\n";
    auto compiled = compile_source(source);
    require(compiled.ok(), "frontend rejected surviving weak target source");
    lang::VM vm;
    vm.set_gc_stress(
        lang::gc::StressConfig{.collect_every_n_instructions = 1,
                               .collect_minor_every_n_instructions = 1});
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Int64 && result.as_i64() == 18,
            "strongly reachable WeakGet target did not survive stress collection");
}

void frontend_accepts_every_declared_weak_object_family() {
    const std::vector<std::string> sources{
        "let x: pair<i64, i64> = pair(1, 2); let w: weak<pair<i64, i64>> = weak(x); w\n",
        "type Node = pair<i64, Node>; let x: Node = pair(1, nil); let w: weak<Node> = weak(x); w\n",
        "let x: [i64] = [1, 2]; let w: weak<[i64]> = weak(x); w\n",
        "let x: map<i64, i64> = map<i64, i64>(); let w: weak<map<i64, i64>> = weak(x); w\n",
        "let x: str = \"weak\"; let w: weak<str> = weak(x); w\n",
        "let x: fn(i64) -> i64 = fn(v: i64) -> i64 { v }; let w: weak<fn(i64) -> i64> = weak(x); w\n",
    };
    for (const auto& source : sources) {
        const auto compiled = compile_source(source);
        require(compiled.ok(),
                "frontend rejected an allowed weak object target: " +
                    (compiled.diagnostics.empty()
                         ? std::string("<no diagnostic>")
                         : compiled.diagnostics.front().message));
    }
}

void frontend_weak_values_compose_through_containers_and_captures() {
    const std::string source =
        "let target: pair<i64, i64> = pair(5, 6);\n"
        "let w: weak<pair<i64, i64>> = weak(target);\n"
        "let holder: pair<weak<pair<i64, i64>>, weak<pair<i64, i64>>> = pair(w, w);\n"
        "let items: [weak<pair<i64, i64>>] = [w, holder.right];\n"
        "let table: map<i64, weak<pair<i64, i64>>> = map<i64, weak<pair<i64, i64>>>();\n"
        "table[1] = items[0];\n"
        "let captured: fn() -> weak<pair<i64, i64>> = fn() -> weak<pair<i64, i64>> { w };\n"
        "let got: pair<i64, i64> = captured().get();\n"
        "let got_table: pair<i64, i64> = table[1].get();\n"
        "let score: i64 = 0;\n"
        "if is_nil(got) {\n"
        "  score = -1;\n"
        "} else {\n"
        "  score = got.left;\n"
        "}\n"
        "if is_nil(got_table) {\n"
        "  score = -2;\n"
        "} else {\n"
        "  score = score + got_table.left;\n"
        "}\n"
        "target = pair(score, 0);\n"
        "score\n";
    const auto compiled = compile_source(source);
    require(compiled.ok(),
            "weak values failed to compose through containers/captures: " +
                (compiled.diagnostics.empty()
                     ? std::string("<no diagnostic>")
                     : compiled.diagnostics.front().message));
    lang::VM vm;
    vm.set_gc_stress(
        lang::gc::StressConfig{.collect_every_n_instructions = 3,
                               .collect_minor_every_n_instructions = 4});
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Int64 && result.as_i64() == 10,
            "weak container/capture composition returned the wrong payload");

    const auto refined_capture = compile_source(
        "let target: pair<i64, i64> = pair(7, 8);\n"
        "let w: weak<pair<i64, i64>> = weak(target);\n"
        "let got: pair<i64, i64> = w.get();\n"
        "let answer: i64 = 0;\n"
        "let read: fn() -> i64 = fn() -> i64 { 0 };\n"
        "if is_nil(got) {\n"
        "  answer = -1;\n"
        "} else {\n"
        "  read = fn() -> i64 { got.left };\n"
        "  answer = read();\n"
        "}\n"
        "answer\n");
    require(refined_capture.ok(),
            "is_nil-refined WeakGet value could not be captured: " +
                (refined_capture.diagnostics.empty()
                     ? std::string("<no diagnostic>")
                     : refined_capture.diagnostics.front().message));
    lang::VM refined_vm;
    const auto refined_result =
        refined_vm.execute(*refined_capture.verified_module);
    require(refined_result.tag() == lang::Value::Tag::Int64 &&
                refined_result.as_i64() == 7,
            "refined WeakGet closure capture lost its target payload");
}

void frontend_rejects_invalid_weak_programs_with_positions() {
    {
        const auto result = compile_source(
            "let w: weak<i64> = weak(1);\n"
            "w\n");
        require(!result.ok() &&
                    diagnostic_contains(result,
                                        "weak target type must be an object type"),
                "weak<i64> was not rejected by the type checker");
        bool positioned = false;
        for (const auto& diagnostic : result.diagnostics) {
            positioned = positioned ||
                         (diagnostic.message.find("weak target type") !=
                              std::string::npos &&
                          diagnostic.position.line == 1 &&
                          diagnostic.position.column == 13);
        }
        require(positioned, "weak<i64> diagnostic was not positioned on i64");
    }
    {
        const auto result = compile_source(
            "let x: pair<i64, i64> = pair(1, 2);\n"
            "let w: weak<pair<i64, i64>> = weak(x);\n"
            "let got: pair<i64, i64> = w.get();\n"
            "got.left\n");
        require(!result.ok() && diagnostic_contains(
                                    result, "requires non-nil value"),
                "unguarded WeakGet result use was not rejected");
    }
    {
        const auto result = compile_source(
            "let x: pair<i64, i64> = pair(1, 2);\n"
            "x.get()\n");
        require(!result.ok() &&
                    diagnostic_contains(result, "get requires weak"),
                "get on a non-weak receiver was not rejected");
    }
    {
        const auto result = compile_source(
            "let w: weak<pair<i64, i64>> = weak(1);\n"
            "w\n");
        require(!result.ok() &&
                    diagnostic_contains(
                        result, "weak() requires a non-nil object operand"),
                "weak(non-object) was not rejected");
    }
    {
        const auto result = compile_source(
            "let target: map<i64, i64> = map<i64, i64>();\n"
            "let w: weak<map<i64, i64>> = weak(target);\n"
            "let got: map<i64, i64> = w.get();\n"
            "got[1] = 2;\n"
            "0\n");
        require(!result.ok() && diagnostic_contains(
                                    result, "assignment requires non-nil value"),
                "unguarded WeakGet map assignment was not rejected");
    }
    {
        const auto result = compile_source(
            "let target: [i64] = [1, 2];\n"
            "let w: weak<[i64]> = weak(target);\n"
            "let got: [i64] = w.get();\n"
            "got[0] = 3;\n"
            "0\n");
        require(!result.ok() && diagnostic_contains(
                                    result, "assignment requires non-nil value"),
                "unguarded WeakGet array assignment was not rejected");
    }
    {
        const auto result = compile_source(
            "let target: map<i64, pair<i64, i64>> = map<i64, pair<i64, i64>>();\n"
            "let w: weak<map<i64, pair<i64, i64>>> = weak(target);\n"
            "let got: map<i64, pair<i64, i64>> = w.get();\n"
            "got[1].left = 4;\n"
            "0\n");
        require(!result.ok() && diagnostic_contains(
                                    result, "assignment requires non-nil value"),
                "unguarded WeakGet nested lvalue assignment was not rejected");
    }
    {
        const auto result = compile_source(
            "let target: pair<i64, i64> = pair(1, 2);\n"
            "let w: weak<pair<i64, i64>> = weak(target);\n"
            "let got: pair<i64, i64> = w.get();\n"
            "let read: fn() -> i64 = fn() -> i64 { got.left };\n"
            "read()\n");
        require(!result.ok() && diagnostic_contains(
                                    result, "captured local 'got' requires non-nil value"),
                "unguarded WeakGet closure capture was not rejected");
    }
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::array tests{
        TestCase{"weak_target_does_not_retain_referent_but_strong_field_does",
                 weak_target_does_not_retain_referent_but_strong_field_does},
        TestCase{"full_compaction_forwards_live_weak_target",
                 full_compaction_forwards_live_weak_target},
        TestCase{"map_growth_relocation_forwards_weak_target",
                 map_growth_relocation_forwards_weak_target},
        TestCase{"old_weak_ref_clears_dead_young_target_in_minor_collection",
                 old_weak_ref_clears_dead_young_target_in_minor_collection},
        TestCase{"old_weak_ref_forwards_strongly_rooted_young_target_in_minor_collection",
                 old_weak_ref_forwards_strongly_rooted_young_target_in_minor_collection},
        TestCase{"cleared_weak_ref_never_resurrects_after_slot_reuse",
                 cleared_weak_ref_never_resurrects_after_slot_reuse},
        TestCase{"weak_ref_to_weak_ref_keeps_only_strongly_rooted_weak_owner_alive",
                 weak_ref_to_weak_ref_keeps_only_strongly_rooted_weak_owner_alive},
        TestCase{"weak_ref_owner_is_a_normal_strong_referent_in_every_container_kind",
                 weak_ref_owner_is_a_normal_strong_referent_in_every_container_kind},
        TestCase{"weak_opcodes_preserve_target_shape_and_is_nil_refinement",
                 weak_opcodes_preserve_target_shape_and_is_nil_refinement},
        TestCase{"weak_get_observes_clearing_in_vm",
                 weak_get_observes_clearing_in_vm},
        TestCase{"verifier_reports_stable_weak_reason_codes",
                 verifier_reports_stable_weak_reason_codes},
        TestCase{"maybe_weak_target_rejoins_after_is_nil_branch",
                 maybe_weak_target_rejoins_after_is_nil_branch},
        TestCase{"frontend_weak_get_refines_and_observes_collection",
                 frontend_weak_get_refines_and_observes_collection},
        TestCase{"frontend_weak_target_survives_when_strongly_reachable",
                 frontend_weak_target_survives_when_strongly_reachable},
        TestCase{"frontend_accepts_every_declared_weak_object_family",
                 frontend_accepts_every_declared_weak_object_family},
        TestCase{"frontend_weak_values_compose_through_containers_and_captures",
                 frontend_weak_values_compose_through_containers_and_captures},
        TestCase{"frontend_rejects_invalid_weak_programs_with_positions",
                 frontend_rejects_invalid_weak_programs_with_positions},
    };

    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
