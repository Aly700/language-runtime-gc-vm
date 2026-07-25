#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"
#include "fuzz_common.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using lang::Value;
using lang::gc::Heap;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

lang::ObjectId allocate_string(Heap& heap, std::string_view text) {
    return heap.allocate_string(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string string_value(const Heap& heap, lang::ObjectId id) {
    const auto bytes = heap.string_bytes(id);
    return std::string(bytes.begin(), bytes.end());
}

std::string diagnostics_listing(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.position.line << ":" << diagnostic.position.column
            << " " << diagnostic.message << "\n";
    }
    return out.str();
}

template <typename Fn>
void require_logic_error(Fn&& action, const std::string& expected,
                         const std::string& context) {
    try {
        action();
    } catch (const std::logic_error& error) {
        require(error.what() == expected,
                context + ": wrong logic_error: " + error.what());
        return;
    }
    throw std::runtime_error(context + ": expected logic_error");
}

lang::Module intern_module() {
    lang::Module module;
    module.string_constants = {"stack-root"};

    lang::Function function;
    function.signature.return_type = lang::ValueKind::Str;
    function.code = {
        {lang::OpCode::PushStr, 0},
        {lang::OpCode::StrIntern, 0},
        {lang::OpCode::Return, 0},
    };
    module.functions.push_back(std::move(function));
    return module;
}

void opcode_is_append_only_and_stack_maps_are_exact() {
    require(static_cast<std::size_t>(lang::OpCode::StrIntern) ==
                static_cast<std::size_t>(lang::OpCode::TailCall) + 1,
            "StrIntern was not appended after TailCall");
    require(static_cast<std::size_t>(
                lang::VerifierReason::StrInternRequiresStr) ==
                static_cast<std::size_t>(
                    lang::VerifierReason::BadTailCallArgKind) +
                    1,
            "StrInternRequiresStr was not appended after the iteration-39 "
            "reason codes");

    auto report = lang::verify_module_with_diagnostics(intern_module());
    require(report.module.has_value(),
            "verifier rejected valid StrIntern bytecode");
    const auto& maps =
        report.module->verification().functions.at(0).stack_maps;
    require(maps.size() == 3, "StrIntern verification emitted wrong map count");
    require(maps.at(0).object_slots.empty(),
            "PushStr entry stack map was not empty");
    require(maps.at(1).object_slots == std::vector<bool>{true},
            "StrIntern did not precisely root its source across allocation");
    require(maps.at(2).object_slots == std::vector<bool>{true},
            "StrIntern result was not a precise string root");

    lang::VM vm;
    const auto result = vm.execute(*report.module);
    require(result.is_object() &&
                string_value(vm.heap(), result.as_object()) == "stack-root",
            "StrIntern bytecode changed string bytes");

    lang::Function invalid;
    invalid.signature.return_type = lang::ValueKind::Str;
    invalid.code = {
        {lang::OpCode::ConstantI64, 7},
        {lang::OpCode::StrIntern, 0},
        {lang::OpCode::Return, 0},
    };
    const auto invalid_report = lang::verify_with_diagnostics(invalid);
    require(!invalid_report.result.has_value() &&
                !invalid_report.diagnostics.empty() &&
                invalid_report.diagnostics.front().reason ==
                    lang::VerifierReason::StrInternRequiresStr,
            "StrIntern non-string operand lacked its stable verifier reason");
}

void live_canonical_deduplicates_in_slot_order_and_graph_shares() {
    Heap heap;
    const auto first_source = allocate_string(heap, "same-bytes");
    auto first = heap.make_handle(
        heap.intern_string(Value::object(first_source)));
    const auto second_source = allocate_string(heap, "same-bytes");
    const auto second =
        heap.intern_string(Value::object(second_source));

    require(second == first.object(),
            "equal bytes did not reuse the live canonical string");
    require(first.object() != first_source &&
                first.object() != second_source,
            "intern miss did not allocate an independent canonical copy");
    require(heap.TEST_ONLY_intern_table_size() == 1,
            "equal bytes created duplicate intern entries");
    require(heap.TEST_ONLY_intern_table_target(0) == first.object(),
            "intern table did not name the live canonical");

    auto shared = heap.make_handle(heap.allocate_pair(
        Value::object(first.object()), Value::object(second)));
    const auto graph =
        fuzz::canonical_object_graph(heap, shared.object());
    require(graph.find("@0 = pair(@1, @1)") != std::string::npos,
            "canonical graph oracle did not observe interned sharing\n" +
                graph);

    const auto later_source = allocate_string(heap, "different-bytes");
    auto later = heap.make_handle(
        heap.intern_string(Value::object(later_source)));
    require(later.object() != first.object(),
            "unequal bytes incorrectly shared an intern canonical");
    require(heap.TEST_ONLY_intern_table_size() == 2 &&
                heap.TEST_ONLY_intern_table_target(0) == first.object() &&
                heap.TEST_ONLY_intern_table_target(1) == later.object(),
            "intern entries were not kept in deterministic base-slot order");
    heap.TEST_ONLY_validate_gc_invariants();
}

void dead_canonical_is_evicted_and_validator_hook_is_nonvacuous() {
    {
        Heap heap;
        const auto source = allocate_string(heap, "weak-only");
        (void)heap.intern_string(Value::object(source));
        require(heap.TEST_ONLY_intern_table_size() == 1,
                "intern miss did not register a weak entry");

        heap.collect();

        require(heap.live_count() == 0,
                "intern table extended canonical liveness");
        require(heap.TEST_ONLY_intern_table_size() == 0,
                "dead canonical intern entry was not evicted");
        heap.TEST_ONLY_validate_gc_invariants();
    }

    {
        Heap heap;
        const auto source = allocate_string(heap, "validator-proof");
        (void)heap.intern_string(Value::object(source));
        heap.TEST_ONLY_skip_next_intern_table_weak_processing();
        require_logic_error(
            [&] { heap.collect(); },
            "intern table entry names an invalid or stale canonical string",
            "intern-table validator accepted an elided dead-entry eviction");
    }
}

void resurrection_without_collection_reuses_physically_present_canonical() {
    Heap heap;
    const auto source = allocate_string(heap, "resurrection");
    const auto unreachable =
        heap.intern_string(Value::object(source));
    require(heap.TEST_ONLY_intern_table_size() == 1,
            "resurrection setup did not retain a weak table entry");

    const auto query = allocate_string(heap, "resurrection");
    const auto resurrected =
        heap.intern_string(Value::object(query));

    require(resurrected == unreachable,
            "uncleared weak entry did not resurrect its physical canonical");
    auto root = heap.make_handle(resurrected);
    heap.collect();
    require(string_value(heap, root.object()) == "resurrection" &&
                heap.TEST_ONLY_intern_table_target(0) == root.object(),
            "resurrected canonical did not become an ordinary strong root");
    heap.TEST_ONLY_validate_gc_invariants();
}

void atomic_and_minor_collection_forward_or_clear_entries() {
    {
        Heap heap;
        (void)heap.allocate_pair(Value::int64(-1), Value::int64(-2));
        const auto source = allocate_string(heap, "atomic-forward");
        auto canonical = heap.make_handle(
            heap.intern_string(Value::object(source)));
        auto query =
            heap.make_handle(allocate_string(heap, "atomic-forward"));
        const auto old_canonical = canonical.object();

        heap.collect();

        require(canonical.object() != old_canonical,
                "atomic-compaction setup did not move the canonical");
        require(heap.TEST_ONLY_intern_table_target(0) ==
                    canonical.object(),
                "atomic compaction did not forward the intern entry");
        require(heap.intern_string(query.value()) == canonical.object(),
                "post-movement intern missed the forwarded canonical");
        heap.TEST_ONLY_validate_gc_invariants();
    }

    {
        Heap heap;
        (void)heap.allocate_pair(Value::int64(-3), Value::int64(-4));
        const auto source = allocate_string(heap, "minor-forward");
        auto canonical = heap.make_handle(
            heap.intern_string(Value::object(source)));

        heap.collect_minor();

        require(heap.TEST_ONLY_is_old_object(canonical.object()),
                "minor collection did not promote the rooted canonical");
        require(heap.TEST_ONLY_intern_table_target(0) ==
                    canonical.object(),
                "minor collection did not forward the intern entry");
        const auto query = allocate_string(heap, "minor-forward");
        require(heap.intern_string(Value::object(query)) ==
                    canonical.object(),
                "post-minor intern missed the promoted canonical");
        heap.TEST_ONLY_validate_gc_invariants();
    }

    {
        Heap heap;
        const auto source = allocate_string(heap, "minor-clear");
        (void)heap.intern_string(Value::object(source));
        heap.collect_minor();
        require(heap.live_count() == 0 &&
                    heap.TEST_ONLY_intern_table_size() == 0,
                "minor collection retained a weak-only young canonical");
    }
}

void map_growth_relocation_preserves_intern_entries() {
    Heap heap;
    const auto source = allocate_string(heap, "growth-safe");
    auto canonical = heap.make_handle(
        heap.intern_string(Value::object(source)));
    auto map = heap.make_handle(heap.allocate_map(12, false, false));
    heap.map_set(map.object(), Value::int64(0), Value::int64(10));
    (void)heap.allocate_pair(Value::int64(-5), Value::int64(-6));
    const auto old_map = map.object();

    heap.map_set(map.object(), Value::int64(8), Value::int64(20));

    require(map.object() != old_map,
            "blocked map-growth setup did not relocate its owner");
    require(heap.TEST_ONLY_intern_table_target(0) == canonical.object(),
            "map-growth relocation corrupted the intern table");
    const auto query = allocate_string(heap, "growth-safe");
    require(heap.intern_string(Value::object(query)) ==
                canonical.object(),
            "intern lookup failed after map-growth relocation");
    heap.TEST_ONLY_validate_gc_invariants();
}

void run_mid_phase_intern_test(bool begin_with_incremental_marking) {
    Heap heap;
    (void)heap.allocate_pair(Value::int64(-7), Value::int64(-8));
    const auto source = allocate_string(heap, "incremental-forward");
    auto canonical = heap.make_handle(
        heap.intern_string(Value::object(source)));
    auto query =
        heap.make_handle(allocate_string(heap, "incremental-forward"));
    auto tail = heap.make_handle(
        heap.allocate_pair(Value::int64(1), Value::int64(2)));
    const auto old_canonical = canonical.object();

    if (begin_with_incremental_marking) {
        heap.start_incremental_marking();
        while (!heap.incremental_marking_quiescent()) {
            (void)heap.incremental_mark_step(1);
        }
        heap.finish_incremental_marking_to_incremental_compaction();
    } else {
        heap.start_incremental_compaction();
    }

    require(heap.incremental_compact_step(1) == 1,
            "budget-one compaction did not relocate the canonical");
    require(heap.incremental_compaction_active() &&
                !heap.incremental_compaction_quiescent(),
            "incremental setup did not pause in a live mid-phase state");
    require(canonical.object() != old_canonical,
            "incremental setup did not move the canonical first");
    require(heap.TEST_ONLY_intern_table_target(0) ==
                canonical.object(),
            "per-object relocation did not forward the intern entry");

    const auto observed = heap.intern_string(query.value());

    require(!heap.incremental_compaction_active(),
            "mutator-time intern did not finish active compaction");
    require(observed == canonical.object(),
            "mid-phase intern missed the forwarded canonical");
    require(heap.left(tail.object()).as_i64() == 1,
            "finishing compaction for intern lost a later survivor");
    heap.TEST_ONLY_validate_gc_invariants();
}

void incremental_compaction_forwards_before_mid_phase_intern() {
    run_mid_phase_intern_test(false);
    run_mid_phase_intern_test(true);
}

void incremental_marking_hit_greys_weak_canonical() {
    Heap heap;
    const auto source = allocate_string(heap, "mark-publication");
    const auto canonical =
        heap.intern_string(Value::object(source));
    const auto query = allocate_string(heap, "mark-publication");

    heap.start_incremental_marking();
    require(heap.incremental_marking_quiescent(),
            "rootless incremental marking setup was not quiescent");

    const auto observed =
        heap.intern_string(Value::object(query));

    require(observed == canonical,
            "incremental marking hit did not return the weak canonical");
    require(heap.incremental_mark_step(1) == 1,
            "weak-to-strong intern publication did not grey the canonical");
    auto root = heap.make_handle(observed);
    heap.finish_incremental_marking();
    require(string_value(heap, root.object()) == "mark-publication" &&
                heap.TEST_ONLY_intern_table_target(0) == root.object(),
            "published canonical did not survive incremental completion");
    heap.TEST_ONLY_validate_gc_invariants();
}

std::size_t count_opcode(const lang::Module& module, lang::OpCode opcode) {
    std::size_t count = 0;
    for (const auto& function : module.functions) {
        count += static_cast<std::size_t>(std::count_if(
            function.code.begin(), function.code.end(),
            [&](const lang::Instruction& instruction) {
                return instruction.op == opcode;
            }));
    }
    return count;
}

void frontend_composes_with_maps_records_and_captures() {
    const std::string source = R"SRC(
record Box { value: str }

fn hold(value: str) -> fn() -> str {
  fn() -> str { value }
}

let first: str = intern("shared");
let second: str = intern("sha" + "red");
let values: map<str, str> = map<str, str>();
values[first] = intern("payload");
let box: Box = Box { value: second };
let capture: fn() -> str = hold(first);
let captured: str = capture();
print(values[box.value]);
print(captured);
pair(first, pair(box.value, captured))
)SRC";
    const auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(),
            "intern composition source did not compile\n" +
                diagnostics_listing(compiled.diagnostics));
    require(count_opcode(compiled.verified_module->module(),
                         lang::OpCode::StrIntern) == 3,
            "frontend did not lower every intern expression to StrIntern");

    const auto all_schedules = fuzz::schedules();
    for (const auto* schedule_name :
         {"no_stress", "combined_mark_compact"}) {
        const auto& schedule =
            fuzz::find_schedule(all_schedules, schedule_name);
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        const auto result = vm.execute(*compiled.verified_module);
        require(result.is_object(),
                std::string("intern composition returned non-object under ") +
                    schedule_name);
        const auto first_value = vm.heap().left(result.as_object());
        const auto nested = vm.heap().right(result.as_object());
        require(first_value.is_object() && nested.is_object(),
                "intern composition returned malformed pair graph");
        const auto record_value =
            vm.heap().left(nested.as_object());
        const auto captured =
            vm.heap().right(nested.as_object());
        require(record_value.is_object() && captured.is_object() &&
                    record_value.as_object() == first_value.as_object() &&
                    captured.as_object() == first_value.as_object(),
                std::string("map/record/capture composition lost canonical "
                            "sharing under ") +
                    schedule_name);
        require(std::string(vm.output().begin(), vm.output().end()) ==
                    "payload\nshared\n",
                std::string("intern composition output drifted under ") +
                    schedule_name);
        vm.heap().TEST_ONLY_validate_gc_invariants();
    }
}

void frontend_rejects_non_string_intern_operands() {
    const std::array<std::string, 4> mutants{
        "intern(1)\n",
        "intern(true)\n",
        "intern(pair(1, 2))\n",
        "intern(nil)\n",
    };
    for (const auto& source : mutants) {
        const auto compiled = lang::frontend::compile_program(source);
        require(!compiled.ok() && !compiled.diagnostics.empty(),
                "non-string intern mutant unexpectedly compiled\n" + source);
        const auto found = std::find_if(
            compiled.diagnostics.begin(), compiled.diagnostics.end(),
            [](const lang::frontend::Diagnostic& diagnostic) {
                return diagnostic.position.line > 0 &&
                       diagnostic.position.column > 0 &&
                       diagnostic.message.find("intern expects str") !=
                           std::string::npos;
            });
        require(found != compiled.diagnostics.end(),
                "non-string intern mutant lacked positioned stable diagnostic\n" +
                    source + diagnostics_listing(compiled.diagnostics));
    }

    const std::string maybe_nil = R"SRC(
let source: str = "x";
let weak_source: weak<str> = weak(source);
intern(weak_source.get())
)SRC";
    const auto compiled =
        lang::frontend::compile_program(maybe_nil);
    require(!compiled.ok() && !compiled.diagnostics.empty(),
            "maybe-nil string intern unexpectedly compiled");
    const auto non_nil = std::find_if(
        compiled.diagnostics.begin(), compiled.diagnostics.end(),
        [](const lang::frontend::Diagnostic& diagnostic) {
            return diagnostic.position.line > 0 &&
                   diagnostic.position.column > 0 &&
                   diagnostic.message.find(
                       "intern requires non-nil str") !=
                       std::string::npos;
        });
    require(non_nil != compiled.diagnostics.end(),
            "maybe-nil intern lacked its positioned refinement diagnostic\n" +
                diagnostics_listing(compiled.diagnostics));
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"opcode_is_append_only_and_stack_maps_are_exact",
         opcode_is_append_only_and_stack_maps_are_exact},
        {"live_canonical_deduplicates_in_slot_order_and_graph_shares",
         live_canonical_deduplicates_in_slot_order_and_graph_shares},
        {"dead_canonical_is_evicted_and_validator_hook_is_nonvacuous",
         dead_canonical_is_evicted_and_validator_hook_is_nonvacuous},
        {"resurrection_without_collection_reuses_physically_present_canonical",
         resurrection_without_collection_reuses_physically_present_canonical},
        {"atomic_and_minor_collection_forward_or_clear_entries",
         atomic_and_minor_collection_forward_or_clear_entries},
        {"map_growth_relocation_preserves_intern_entries",
         map_growth_relocation_preserves_intern_entries},
        {"incremental_compaction_forwards_before_mid_phase_intern",
         incremental_compaction_forwards_before_mid_phase_intern},
        {"incremental_marking_hit_greys_weak_canonical",
         incremental_marking_hit_greys_weak_canonical},
        {"frontend_composes_with_maps_records_and_captures",
         frontend_composes_with_maps_records_and_captures},
        {"frontend_rejects_non_string_intern_operands",
         frontend_rejects_non_string_intern_operands},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << "\n"
                      << error.what() << "\n";
        }
    }
    if (failures != 0) {
        std::cerr << failures
                  << " iteration-44 string-interning test(s) failed\n";
        return 1;
    }
    return 0;
}
