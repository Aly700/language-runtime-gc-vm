#include "lang/gc/heap.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using lang::Value;
using lang::gc::Heap;

struct VectorRoots final : lang::gc::RootProvider {
    std::vector<Value> values;

    void trace_roots(lang::gc::RootVisitor& visitor) override {
        for (auto& value : values) {
            visitor.visit(value);
        }
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_logic_error(Fn&& action, const std::string& message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, message);
}

template <typename Fn>
void require_out_of_range(Fn&& action, const std::string& message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    require(rejected, message);
}

void empty_cycle_obeys_phase_and_budget_contract() {
    Heap heap;

    heap.start_incremental_compaction();
    require(heap.incremental_compaction_active(),
            "incremental compaction did not become active");
    require(heap.incremental_compaction_quiescent(),
            "empty incremental compaction was not quiescent");
    require(heap.incremental_compact_step(0) == 0,
            "zero compaction budget consumed relocation work");
    heap.finish_incremental_compaction();

    require(!heap.incremental_compaction_active(),
            "incremental compaction remained active after finish");
    const auto metrics = heap.metrics();
    require(metrics.incremental_compaction_cycles_started == 1,
            "incremental compaction start metric changed");
    require(metrics.incremental_compaction_steps == 1,
            "incremental compaction step metric changed");
    require(metrics.incremental_compaction_budget_requested == 0,
            "zero budget changed the requested-unit metric");
    require(metrics.incremental_compaction_objects_relocated == 0,
            "empty cycle relocated an object");
    require(metrics.incremental_compaction_final_pauses == 1,
            "incremental compaction final-pause metric changed");
    require(metrics.incremental_compaction_differential_validations == 1,
            "empty cycle skipped differential validation");
}

void phase_conflicts_trap_loudly() {
    {
        Heap heap;
        heap.start_incremental_compaction();
        require_logic_error(
            [&] { heap.start_incremental_compaction(); },
            "starting an active compaction cycle did not trap");
        require_logic_error(
            [&] { heap.start_incremental_marking(); },
            "starting marking during compaction did not trap");
        heap.finish_incremental_compaction();
    }

    {
        Heap heap;
        heap.start_incremental_marking();
        require_logic_error(
            [&] { heap.start_incremental_compaction(); },
            "starting compaction during marking did not trap");
        heap.finish_incremental_marking();
    }

    {
        Heap heap;
        require_logic_error(
            [&] { (void)heap.incremental_compact_step(1); },
            "stepping an idle compaction cycle did not trap");
        require_logic_error(
            [&] { heap.finish_incremental_compaction(); },
            "finishing an idle compaction cycle did not trap");
    }
}

void moved_object_is_accessible_before_its_peer_moves() {
    Heap heap;
    (void)heap.allocate_pair(Value::int64(-1), Value::int64(-1));
    const auto owner_id =
        heap.allocate_pair(Value::nil(), Value::int64(10));
    const auto peer_id =
        heap.allocate_pair(Value::int64(20), Value::int64(30));
    heap.set_left(owner_id, Value::object(peer_id));
    auto root = heap.make_handle(owner_id);
    auto peer_root = heap.make_handle(peer_id);

    heap.start_incremental_compaction();
    require(heap.incremental_compact_step(1) == 1,
            "one-unit compaction budget did not relocate exactly one object");
    require(root.object() != owner_id,
            "handle root was not rewritten when its object moved");
    require(heap.left(owner_id).is_object() &&
                heap.left(owner_id).as_object() == peer_id,
            "forwarding read barrier could not reach the moved owner");
    require(heap.left(peer_id).as_i64() == 20,
            "unmoved peer was inaccessible while its owner was moved");

    heap.set_right(owner_id, Value::int64(99));
    require(heap.right(root.object()).as_i64() == 99,
            "write through a forwarded id missed the relocated object");
    require(heap.incremental_compact_step(1) == 1,
            "peer relocation did not consume one complete-object unit");
    require(heap.left(owner_id).as_object() == peer_root.object(),
            "object-valued accessor did not canonicalize a forwarded peer");

    heap.finish_incremental_compaction();
    require(heap.left(root.object()).is_object(),
            "final rewrite lost the moved owner's peer");
    require(heap.left(heap.left(root.object()).as_object()).as_i64() == 20,
            "peer contents changed after incremental compaction");
}

void mixed_width_plan_consumes_exact_object_budgets() {
    Heap heap;
    (void)heap.allocate_pair(Value::int64(-1), Value::int64(-1));

    const auto pair =
        heap.allocate_pair(Value::int64(1), Value::int64(2));
    const auto scalar0 = heap.allocate_scalar_array(0, 0);
    const auto scalar4 = heap.allocate_scalar_array(4, 7);
    const auto refs = heap.allocate_ref_array(3, Value::object(pair));
    const std::array<std::uint8_t, 5> bytes{'h', 'e', 'l', 'l', 'o'};
    const auto string = heap.allocate_string(bytes);
    const auto closure = heap.allocate_closure(
        4, 9, {Value::object(pair), Value::int64(77)}, {true, false});
    const auto map = heap.allocate_map(2, false, false);
    heap.map_set(map, Value::int64(5), Value::int64(6));
    const auto weak = heap.allocate_weak(Value::object(pair));
    const auto record = heap.allocate_record(
        3, {Value::object(pair), Value::int64(88)}, {true, false});
    const auto variant = heap.allocate_variant(
        5, 1, {Value::int64(99), Value::object(pair)},
        {{true, false}, {false, true}});
    const auto ephemeron =
        heap.allocate_ephemeron(Value::object(pair), Value::int64(100), false);

    VectorRoots roots;
    roots.values = {
        Value::object(pair),       Value::object(scalar0),
        Value::object(scalar4),    Value::object(refs),
        Value::object(string),     Value::object(closure),
        Value::object(map),        Value::object(weak),
        Value::object(record),     Value::object(variant),
        Value::object(ephemeron),
    };
    heap.set_root_provider(&roots);

    const auto live_before = roots.values.size();
    const auto validations_before = heap.TEST_ONLY_validation_count();
    heap.start_incremental_compaction();
    require(heap.incremental_compact_step(0) == 0,
            "zero budget moved a mixed-width survivor");
    const std::array<std::size_t, 4> budgets{3, 1, 7, 3};
    std::size_t schedule_index = 0;
    std::size_t consumed_total = 0;
    while (!heap.incremental_compaction_quiescent()) {
        const auto budget = budgets[schedule_index++ % budgets.size()];
        const auto consumed = heap.incremental_compact_step(budget);
        require(consumed > 0 && consumed <= budget,
                "compaction step exceeded its complete-object budget");
        consumed_total += consumed;
    }
    require(consumed_total == live_before,
            "mixed-width plan did not relocate every survivor exactly once");
    require(heap.incremental_compact_step(100) == 0,
            "oversized budget consumed work after quiescence");
    heap.finish_incremental_compaction();

    require(heap.live_count() == live_before,
            "mixed-width compaction changed the live object count");
    require(heap.metrics().incremental_compaction_objects_relocated ==
                live_before,
            "relocation metric did not count complete objects");
    require(heap.TEST_ONLY_validation_count() >=
                validations_before + live_before + 3,
            "boundary validators did not run throughout relocation");
    require(heap.array_length(roots.values[1].as_object()) == 0 &&
                heap.array_length(roots.values[2].as_object()) == 4,
            "scalar-array widths changed during relocation");
    require(heap.ref_array_length(roots.values[3].as_object()) == 3,
            "reference-array width changed during relocation");
    require(heap.string_length(roots.values[4].as_object()) == bytes.size(),
            "string width changed during relocation");
    require(heap.closure_capture_count(roots.values[5].as_object()) == 2,
            "closure width changed during relocation");
    require(heap.map_length(roots.values[6].as_object()) == 1,
            "map width changed during relocation");
    require(heap.weak_get(roots.values[7].as_object()).is_object(),
            "live weak target was lost during relocation");
    require(heap.record_field_count(roots.values[8].as_object()) == 2,
            "record width changed during relocation");
    require(heap.variant_field_count(roots.values[9].as_object()) == 2,
            "variant width changed during relocation");
    require(heap.ephemeron_key(roots.values[10].as_object()).is_object(),
            "active ephemeron key was lost during relocation");
}

void stale_generation_never_matches_phase_forwarding() {
    Heap heap;
    const auto stale =
        heap.allocate_pair(Value::int64(-10), Value::int64(-20));
    heap.collect();
    require_out_of_range([&] { (void)heap.left(stale); },
                         "dead id was not stale before compaction");

    (void)heap.allocate_pair(Value::int64(-30), Value::int64(-40));
    const auto survivor =
        heap.allocate_pair(Value::int64(31), Value::int64(41));
    auto root = heap.make_handle(survivor);
    heap.start_incremental_compaction();
    require_out_of_range([&] { (void)heap.left(stale); },
                         "stale destination-slot id resolved during preparation");
    require(heap.incremental_compact_step(1) == 1,
            "survivor did not relocate into the stale id's slot");
    require_out_of_range([&] { (void)heap.left(stale); },
                         "stale generation matched the forwarding barrier");
    require(heap.left(survivor).as_i64() == 31,
            "exact current-cycle source id did not resolve while active");
    heap.finish_incremental_compaction();
    require_out_of_range([&] { (void)heap.left(stale); },
                         "stale id became valid after finalization");
    require(heap.left(root.object()).as_i64() == 31,
            "canonical destination root became stale after finalization");
}

void promotion_barrier_tracks_unmoved_young_peer() {
    Heap heap;
    (void)heap.allocate_pair(Value::int64(-1), Value::int64(-1));
    const auto owner =
        heap.allocate_pair(Value::nil(), Value::int64(1));
    const auto peer =
        heap.allocate_pair(Value::int64(2), Value::int64(3));
    heap.set_left(owner, Value::object(peer));
    auto root = heap.make_handle(owner);

    heap.start_incremental_compaction();
    require(heap.incremental_compact_step(1) == 1,
            "owner did not relocate first");
    require(heap.TEST_ONLY_is_old_object(root.object()),
            "relocated owner was not promoted");
    require(heap.TEST_ONLY_is_young_object(peer),
            "unmoved peer was promoted too early");
    require(heap.TEST_ONLY_remembered_set_size() == 1,
            "promotion-created old-to-young edge was not remembered");

    const auto barriers_before = heap.metrics().write_barrier_hits;
    heap.set_right(owner, Value::object(peer));
    require(heap.right(root.object()).as_object() == peer,
            "mid-phase old-to-young store was not published");
    require(heap.metrics().write_barrier_hits == barriers_before + 1,
            "mid-phase old-to-young store missed its write barrier");
    require(heap.incremental_compact_step(1) == 1,
            "peer did not relocate second");
    heap.finish_incremental_compaction();
    require(heap.TEST_ONLY_remembered_set_size() == 0,
            "final pruning retained an all-old remembered edge");
}

void width_changing_operations_finish_the_phase() {
    {
        Heap heap;
        (void)heap.allocate_pair(Value::int64(-1), Value::int64(-1));
        const auto source =
            heap.allocate_pair(Value::int64(7), Value::int64(8));
        auto root = heap.make_handle(source);
        heap.start_incremental_compaction();
        require(heap.incremental_compact_step(1) == 1,
                "source did not move before allocation boundary");
        const auto new_pair =
            heap.allocate_pair(Value::object(source), Value::int64(9));
        require(!heap.incremental_compaction_active(),
                "allocation did not finish active compaction");
        require(heap.left(new_pair).as_object() == root.object(),
                "allocation boundary lost a forwarded constructor operand");
    }

    {
        Heap heap;
        (void)heap.allocate_pair(Value::int64(-1), Value::int64(-1));
        const auto map = heap.allocate_map(0, false, false);
        heap.map_set(map, Value::int64(1), Value::int64(10));
        auto root = heap.make_handle(map);
        heap.start_incremental_compaction();
        require(heap.incremental_compact_step(1) == 1,
                "map did not move before update boundary");
        heap.map_set(map, Value::int64(1), Value::int64(11));
        require(heap.incremental_compaction_active(),
                "fixed-width existing-key update ended compaction");
        heap.map_set(map, Value::int64(2), Value::int64(20));
        require(!heap.incremental_compaction_active(),
                "new map entry did not finish active compaction");
        require(heap.map_get(root.object(), Value::int64(1)).as_i64() == 11 &&
                    heap.map_get(root.object(), Value::int64(2)).as_i64() == 20,
                "map boundary lost an existing or inserted value");
    }

    {
        Heap heap;
        (void)heap.allocate_pair(Value::int64(-1), Value::int64(-1));
        auto root = heap.make_handle(
            heap.allocate_pair(Value::int64(12), Value::int64(13)));
        heap.start_incremental_compaction();
        heap.collect_minor();
        require(!heap.incremental_compaction_active(),
                "atomic minor collection did not finish active compaction");
        require(heap.left(root.object()).as_i64() == 12,
                "collection boundary lost the rooted survivor");
    }
}

void differential_validator_is_nonvacuous() {
    {
        Heap heap;
        (void)heap.allocate_pair(Value::int64(-1), Value::int64(-1));
        auto root = heap.make_handle(
            heap.allocate_pair(Value::int64(51), Value::int64(52)));
        heap.start_incremental_compaction();
        heap.TEST_ONLY_corrupt_next_incremental_compaction_copy();
        require(heap.incremental_compact_step(1) == 1,
                "corruption probe did not relocate its survivor");
        heap.set_right(root.object(), Value::int64(53));
        require_logic_error(
            [&] { heap.finish_incremental_compaction(); },
            "shadow STW validator accepted a corrupted relocation copy");
        require(heap.metrics()
                        .incremental_compaction_differential_validations == 0,
                "failed differential validation was counted as successful");
        require(heap.left(root.object()).as_i64() != 51,
                "test-only hook did not make the production copy observably wrong");
    }

    {
        Heap heap;
        (void)heap.allocate_pair(Value::int64(-1), Value::int64(-1));
        auto root = heap.make_handle(
            heap.allocate_pair(Value::int64(51), Value::int64(52)));
        heap.start_incremental_compaction();
        heap.finish_incremental_compaction();
        require(heap.left(root.object()).as_i64() == 51,
                "uncorrupted control changed the survivor");
        require(heap.metrics()
                        .incremental_compaction_differential_validations == 1,
                "uncorrupted control skipped differential validation");
    }
}

void vm_instruction_boundaries_drive_compaction_and_combined_phases() {
    const auto compiled = lang::frontend::compile_program(R"(
variant Error { Bad(pair<i64, i64>) }
fn fail(value: pair<i64, i64>) -> i64 {
  throw Error.Bad(value);
  0
}
fn caller(value: pair<i64, i64>) -> i64 {
  fail(value)
}
let anchor: pair<i64, i64> = pair(40, 2);
let answer: i64 = 0;
try {
  answer = caller(anchor);
} catch (error: Error) {
  match error { Bad(saved) => { answer = saved.left + saved.right; } }
}
print(to_str(answer));
answer
)");
    require(compiled.ok(), "incremental compaction VM source did not compile");

    const auto run = [&](bool combined) {
        lang::VM vm;
        lang::gc::StressConfig stress;
        stress.incremental_compact_step_budgets = {1, 3};
        if (combined) {
            stress.incremental_mark_step_budgets = {1};
        }
        vm.set_gc_stress(stress);
        const auto result = vm.execute(*compiled.verified_module);
        require(result.as_i64() == 42,
                "instruction-boundary compaction changed VM result");
        require(std::string(vm.output().begin(), vm.output().end()) == "42\n",
                "instruction-boundary compaction changed output");
        require(!vm.heap().incremental_compaction_active() &&
                    !vm.heap().incremental_marking_active(),
                "VM returned with an incremental phase active");
        require(vm.metrics().heap.incremental_compaction_cycles_started > 0 &&
                    vm.metrics()
                            .heap
                            .incremental_compaction_differential_validations >
                        0,
                "VM schedule did not complete validated compaction");
    };

    run(false);
    run(true);

    {
        lang::VM vm;
        lang::gc::StressConfig invalid;
        invalid.incremental_compact_step_budgets = {1, 0};
        bool rejected = false;
        try {
            vm.set_gc_stress(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "zero compaction stress budget was accepted");
    }

    {
        const auto trapping = lang::frontend::compile_program(
            "let text: str = \"not-an-i64\"; to_i64(text)\n");
        require(trapping.ok(), "runtime-trap probe did not compile");
        lang::VM vm;
        lang::gc::StressConfig stress;
        stress.incremental_compact_step_budgets = {1};
        vm.set_gc_stress(stress);
        bool trapped = false;
        try {
            (void)vm.execute(*trapping.verified_module);
        } catch (const std::runtime_error&) {
            trapped = true;
        }
        require(trapped, "runtime-trap probe did not trap");
        require(!vm.heap().incremental_compaction_active() &&
                    !vm.heap().incremental_marking_active(),
                "runtime trap escaped with an incremental phase active");
    }
}

void precise_layouts_roots_and_registries_survive_mid_phase() {
    Heap heap;
    const auto dead =
        heap.allocate_pair(Value::int64(-1), Value::int64(-2));
    const auto target =
        heap.allocate_pair(Value::int64(10), Value::int64(11));
    const auto peer =
        heap.allocate_pair(Value::int64(20), Value::int64(21));
    const auto scalars = heap.allocate_scalar_array(2, 0);
    heap.array_set(scalars, 0, static_cast<std::int64_t>(dead));
    const auto refs = heap.allocate_ref_array(2, Value::object(peer));
    const std::array<std::uint8_t, 3> key_bytes{'k', 'e', 'y'};
    const auto key = heap.allocate_string(key_bytes);
    const auto closure = heap.allocate_closure(
        8, 3,
        {Value::object(target),
         Value::int64(static_cast<std::int64_t>(dead))},
        {true, false});
    const auto map = heap.allocate_map(9, true, true);
    heap.map_set(map, Value::object(key), Value::object(peer));
    const auto weak = heap.allocate_weak(Value::object(peer));
    const auto record = heap.allocate_record(
        10,
        {Value::object(peer),
         Value::int64(static_cast<std::int64_t>(dead))},
        {true, false});
    const auto variant = heap.allocate_variant(
        11, 1,
        {Value::int64(static_cast<std::int64_t>(dead)),
         Value::object(peer)},
        {{true, false}, {false, true}});
    const auto ephemeron = heap.allocate_ephemeron(
        Value::object(target), Value::object(peer), true);

    VectorRoots roots;
    roots.values = {
        Value::object(target),    Value::object(peer),
        Value::object(scalars),   Value::object(refs),
        Value::object(closure),   Value::object(map),
        Value::object(weak),      Value::object(record),
        Value::object(variant),   Value::object(ephemeron),
    };
    heap.set_root_provider(&roots);
    heap.start_incremental_compaction();
    require(heap.incremental_compact_step(1) == 1,
            "first precise-layout survivor did not move");
    require(roots.values[0].as_object() != target &&
                roots.values[1].as_object() == peer,
            "explicit root provider did not rewrite exactly the moved prefix");
    require(heap.weak_get(weak).as_object() == peer,
            "unmoved weak owner could not read its live target");
    require(heap.ephemeron_key(ephemeron).as_object() ==
                roots.values[0].as_object(),
            "ephemeron key accessor did not canonicalize a moved target");

    heap.array_set(scalars, 1, 404);
    heap.ref_array_set(refs, 0, Value::object(target));
    heap.map_set(map, Value::object(key), Value::object(target));
    heap.record_set(record, 0, Value::object(target));
    heap.record_set(record, 1, Value::int64(505));
    heap.ephemeron_set_value(ephemeron, Value::object(target));
    heap.finish_incremental_compaction();

    require(heap.array_get(roots.values[2].as_object(), 0) ==
                static_cast<std::int64_t>(dead) &&
                heap.array_get(roots.values[2].as_object(), 1) == 404,
            "scalar array interpreted ObjectId-shaped bits as references");
    require(heap.closure_capture(roots.values[4].as_object(), 1).as_i64() ==
                static_cast<std::int64_t>(dead),
            "closure scalar capture was traced or rewritten");
    require(heap.record_get(roots.values[7].as_object(), 1).as_i64() == 505,
            "record scalar slot or mid-phase mutation changed");
    require(heap.variant_get(roots.values[8].as_object(), 0).as_i64() ==
                static_cast<std::int64_t>(dead),
            "variant scalar slot was traced or rewritten");
    require(heap.ref_array_get(roots.values[3].as_object(), 0).as_object() ==
                roots.values[0].as_object() &&
                heap.map_get(roots.values[5].as_object(),
                             heap.map_key_at(
                                 roots.values[5].as_object(), 0))
                        .as_object() == roots.values[0].as_object() &&
                heap.record_get(roots.values[7].as_object(), 0).as_object() ==
                    roots.values[0].as_object() &&
                heap.ephemeron_value(roots.values[9].as_object()).as_object() ==
                    roots.values[0].as_object(),
            "mapped references or mirrored mutations lost canonical targets");
    require_out_of_range([&] { (void)heap.object(dead); },
                         "scalar ObjectId-shaped bits retained a dead object");
}

} // namespace

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests{
        {"empty_cycle_obeys_phase_and_budget_contract",
         empty_cycle_obeys_phase_and_budget_contract},
        {"phase_conflicts_trap_loudly", phase_conflicts_trap_loudly},
        {"moved_object_is_accessible_before_its_peer_moves",
         moved_object_is_accessible_before_its_peer_moves},
        {"mixed_width_plan_consumes_exact_object_budgets",
         mixed_width_plan_consumes_exact_object_budgets},
        {"stale_generation_never_matches_phase_forwarding",
         stale_generation_never_matches_phase_forwarding},
        {"promotion_barrier_tracks_unmoved_young_peer",
         promotion_barrier_tracks_unmoved_young_peer},
        {"width_changing_operations_finish_the_phase",
         width_changing_operations_finish_the_phase},
        {"differential_validator_is_nonvacuous",
         differential_validator_is_nonvacuous},
        {"vm_instruction_boundaries_drive_compaction_and_combined_phases",
         vm_instruction_boundaries_drive_compaction_and_combined_phases},
        {"precise_layouts_roots_and_registries_survive_mid_phase",
         precise_layouts_roots_and_registries_survive_mid_phase},
    };

    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
