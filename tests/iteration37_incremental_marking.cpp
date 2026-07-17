#include "lang/gc/heap.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using lang::Value;
using lang::gc::Heap;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void deterministic_budgeted_steps_finish_collection() {
    Heap heap;
    auto leaf = heap.allocate_pair(Value::int64(1), Value::int64(2));
    auto root = heap.make_handle(
        heap.allocate_pair(Value::object(leaf), Value::int64(3)));

    heap.start_incremental_marking();
    require(heap.incremental_marking_active(), "incremental cycle did not start");
    require(heap.incremental_mark_step(0) == 0, "zero budget consumed work");
    require(heap.incremental_mark_step(1) == 1, "one-unit budget was not exact");
    heap.finish_incremental_marking();

    require(!heap.incremental_marking_active(), "incremental cycle remained active");
    require(heap.live_count() == 2, "incremental marking lost a reachable object");
    require(heap.left(root.object()).is_object(), "root edge was not forwarded");
}

void black_owner_store_shades_white_target() {
    Heap heap;
    auto owner = heap.make_handle(
        heap.allocate_pair(Value::int64(0), Value::int64(0)));
    auto target = heap.allocate_pair(Value::int64(7), Value::int64(8));

    heap.start_incremental_marking();
    require(heap.incremental_mark_step(1) == 1, "owner was not scanned black");
    heap.set_left(owner.object(), Value::object(target));
    heap.finish_incremental_marking();

    require(heap.left(owner.object()).is_object(),
            "incremental insertion barrier lost published target");
    require(heap.live_count() == 2, "published target did not survive");
}

void ephemerons_wait_for_final_pause() {
    Heap heap;
    auto key = heap.make_handle(
        heap.allocate_pair(Value::int64(1), Value::int64(1)));
    auto value = heap.allocate_pair(Value::int64(2), Value::int64(2));
    auto entry = heap.make_handle(heap.allocate_ephemeron(
        Value::object(key.object()), Value::object(value), true));

    heap.start_incremental_marking();
    (void)heap.incremental_mark_step(100);
    require(heap.metrics().ephemeron_activations == 0,
            "ephemeron activated before final stop-the-world pause");
    heap.finish_incremental_marking();
    require(heap.ephemeron_value(entry.object()).is_object(),
            "final ephemeron fixpoint lost active value");
}

void vm_instruction_budgets_drive_incremental_cycles() {
    const auto compiled = lang::frontend::compile_program(R"(
let a: pair<i64, i64> = pair(1, 2);
let b: pair<pair<i64, i64>, i64> = pair(a, 3);
b.left.left
)");
    require(compiled.ok(), "incremental VM source did not compile");
    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.incremental_mark_step_budgets = {1, 3};
    vm.set_gc_stress(stress);
    require(vm.execute(*compiled.verified_module).as_i64() == 1,
            "incremental VM schedule changed result");
    require(vm.metrics().heap.incremental_cycles_started > 0,
            "VM did not start incremental marking");
    require(!vm.heap().incremental_marking_active(),
            "VM returned with an active incremental cycle");
}

void barrier_elision_is_detected_by_tricolour_validator() {
    Heap heap;
    auto owner = heap.make_handle(
        heap.allocate_pair(Value::int64(0), Value::int64(0)));
    auto target = heap.allocate_pair(Value::int64(9), Value::int64(9));
    heap.start_incremental_marking();
    (void)heap.incremental_mark_step(1);
    heap.TEST_ONLY_skip_next_incremental_write_barrier();
    heap.set_left(owner.object(), Value::object(target));
    bool rejected = false;
    try { heap.TEST_ONLY_validate_incremental_marking(); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "tri-colour validator accepted black-to-white edge");
}

void final_remark_matches_current_stop_the_world_liveness() {
    Heap heap;
    auto discarded = heap.allocate_pair(Value::int64(4), Value::int64(5));
    auto owner = heap.make_handle(
        heap.allocate_pair(Value::object(discarded), Value::int64(0)));
    heap.start_incremental_marking();
    (void)heap.incremental_mark_step(10);
    heap.set_left(owner.object(), Value::nil());
    heap.finish_incremental_marking();
    require(heap.live_count() == 1,
            "incremental result retained object absent from final STW graph");
}
}

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests{
        {"deterministic_budgeted_steps_finish_collection",
         deterministic_budgeted_steps_finish_collection},
        {"black_owner_store_shades_white_target", black_owner_store_shades_white_target},
        {"ephemerons_wait_for_final_pause", ephemerons_wait_for_final_pause},
        {"vm_instruction_budgets_drive_incremental_cycles",
         vm_instruction_budgets_drive_incremental_cycles},
        {"barrier_elision_is_detected_by_tricolour_validator",
         barrier_elision_is_detected_by_tricolour_validator},
        {"final_remark_matches_current_stop_the_world_liveness",
         final_remark_matches_current_stop_the_world_liveness},
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
