#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"
#include "test_support.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr lang::ObjectId kSlotMask = 0xFFFF'FFFFull;
constexpr unsigned kGenerationShift = 32;

static_assert(!std::is_copy_constructible_v<lang::gc::Handle>,
              "GC handles must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<lang::gc::Handle>,
              "GC handles must not be copy-assignable");
static_assert(std::is_move_constructible_v<lang::gc::Handle>,
              "GC handles must be move-constructible");
static_assert(std::is_move_assignable_v<lang::gc::Handle>,
              "GC handles must be move-assignable");

std::uint32_t slot_of(lang::ObjectId id) {
    return static_cast<std::uint32_t>(id & kSlotMask);
}

std::string describe_value(const lang::Value& value) {
    std::ostringstream out;
    switch (value.tag()) {
    case lang::Value::Tag::Int64:
        out << "i64(" << value.as_i64() << ")";
        break;
    case lang::Value::Tag::Bool:
        out << "bool(" << (value.as_bool() ? "true" : "false") << ")";
        break;
    case lang::Value::Tag::Object:
        out << "object(id=" << value.as_object() << ", slot=" << slot_of(value.as_object())
            << ")";
        break;
    case lang::Value::Tag::Nil:
        out << "nil";
        break;
    }
    return out.str();
}

std::string describe_heap_object(const lang::gc::Heap& heap, lang::ObjectId id,
                                 const std::string& label) {
    std::ostringstream out;
    out << label << ": id=" << id << " slot=" << slot_of(id);
    try {
        const auto& object = heap.object(id);
        out << " generation="
            << (heap.TEST_ONLY_is_old_object(id) ? "old" : "young")
            << " left=" << describe_value(object.left)
            << " right=" << describe_value(object.right);
    } catch (const std::exception& e) {
        out << " invalid(" << e.what() << ")";
    }
    out << "\n";
    return out.str();
}

std::string describe_heap(const lang::gc::Heap& heap,
                          const std::vector<std::pair<std::string, lang::ObjectId>>& ids) {
    std::ostringstream out;
    out << "live_count=" << heap.live_count() << " capacity_slots=" << heap.capacity_slots()
        << " remembered_set_size=" << heap.TEST_ONLY_remembered_set_size()
        << " handle_roots=" << heap.TEST_ONLY_handle_root_count()
        << " validations=" << heap.TEST_ONLY_validation_count() << "\n";
    for (const auto& [label, id] : ids) {
        out << describe_heap_object(heap, id, label);
    }
    return out.str();
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

void require_pair_i64(const lang::gc::Heap& heap, lang::ObjectId id, std::int64_t left,
                      std::int64_t right, const std::string& context) {
    const auto& object = heap.object(id);
    require(object.left.as_i64() == left && object.right.as_i64() == right,
            context + "\n" + describe_heap_object(heap, id, "pair"));
}

lang::Function int_result_program(std::int64_t value) {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Int64;
    function.local_count = 0;
    function.code = {
        {lang::OpCode::ConstantI64, value},
        {lang::OpCode::Return, 0},
    };
    return function;
}

void handle_keeps_object_alive_and_rewrites_after_major_collection() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(10), lang::Value::int64(11));
    const auto survivor = heap.allocate_pair(lang::Value::int64(20), lang::Value::int64(21));
    auto handle = heap.make_handle(lang::Value::object(survivor));

    heap.collect();

    const auto moved = handle.value().as_object();
    require(moved != survivor && slot_of(moved) == slot_of(dead),
            "major collection did not rewrite the handle slot to the compacted id\n" +
                describe_heap(heap, {{"dead", dead}, {"survivor", survivor}, {"moved", moved}}));
    require(heap.live_count() == 1,
            "handle should keep exactly one survivor live across major collection\n" +
                describe_heap(heap, {{"moved", moved}}));
    require_pair_i64(heap, moved, 20, 21, "major-moved handle object payload changed");
}

void handle_keeps_young_object_alive_and_rewrites_after_minor_collection() {
    lang::gc::Heap heap;
    auto old = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    auto old_handle = heap.make_handle(old);
    heap.collect_minor();
    old = old_handle.object();
    require(heap.TEST_ONLY_is_old_object(old),
            "setup failed to promote old object\n" + describe_heap(heap, {{"old", old}}));

    const auto dead_young = heap.allocate_pair(lang::Value::int64(30), lang::Value::int64(31));
    const auto young = heap.allocate_pair(lang::Value::int64(40), lang::Value::int64(41));
    auto young_handle = heap.make_handle(young);

    heap.collect_minor();

    const auto moved_young = young_handle.object();
    require(moved_young != young && slot_of(moved_young) == slot_of(dead_young),
            "minor collection did not rewrite the handle slot to the moved young survivor\n" +
                describe_heap(heap, {{"old", old},
                                     {"dead_young", dead_young},
                                     {"young", young},
                                     {"moved_young", moved_young}}));
    require(heap.TEST_ONLY_is_old_object(moved_young),
            "minor survivor rooted by handle was not promoted\n" +
                describe_heap(heap, {{"moved_young", moved_young}}));
    require_pair_i64(heap, moved_young, 40, 41,
                     "minor-moved handle object payload changed");
}

void destroyed_handle_no_longer_roots_its_slot() {
    lang::gc::Heap heap;
    const auto object = heap.allocate_pair(lang::Value::int64(50), lang::Value::int64(51));
    {
        auto handle = heap.make_handle(object);
        require(heap.TEST_ONLY_handle_root_count() == 1,
                "handle did not register exactly one root slot\n" +
                    describe_heap(heap, {{"object", object}}));
        require(handle.object() == object,
                "fresh handle did not expose the original object id\n" +
                    describe_heap(heap, {{"object", object}}));
    }

    require(heap.TEST_ONLY_handle_root_count() == 0,
            "destroyed handle left a dangling registered root slot\n" +
                describe_heap(heap, {{"object", object}}));
    heap.collect();

    require(heap.live_count() == 0,
            "destroyed handle still rooted its former object after collection\n" +
                describe_heap(heap, {{"object", object}}));
    require_throws([&] { (void)heap.object(object); },
                   "object id remained valid after its only handle was destroyed\n" +
                       describe_heap(heap, {{"object", object}}));
}

void stale_raw_id_traps_while_handle_stays_valid_after_move() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(60), lang::Value::int64(61));
    const auto raw = heap.allocate_pair(lang::Value::int64(70), lang::Value::int64(71));
    auto handle = heap.make_handle(raw);

    heap.collect();

    const auto moved = handle.object();
    require(moved != raw && slot_of(moved) == slot_of(dead),
            "test setup did not move the handled object\n" +
                describe_heap(heap, {{"dead", dead}, {"raw", raw}, {"moved", moved}}));
    require_throws([&] { (void)heap.object(raw); },
                   "raw pre-move ObjectId did not trap after moving collection\n" +
                       describe_heap(heap, {{"raw", raw}, {"moved", moved}}));
    require_pair_i64(heap, moved, 70, 71,
                     "handle did not keep the moved object dereferenceable");
}

void handles_survive_heap_and_vm_stress_modes() {
    {
        lang::gc::Heap heap;
        const auto dead = heap.allocate_pair(lang::Value::int64(80), lang::Value::int64(81));
        const auto survivor = heap.allocate_pair(lang::Value::int64(82), lang::Value::int64(83));
        auto handle = heap.make_handle(survivor);
        lang::gc::StressConfig stress;
        stress.collect_before_every_allocation = true;
        heap.set_stress_config(stress);

        const auto allocated = heap.allocate_pair(lang::Value::int64(84), lang::Value::int64(85));
        const auto moved = handle.object();
        require(moved != survivor && slot_of(moved) == slot_of(dead),
                "before-every-allocation stress did not trace handle roots\n" +
                    describe_heap(heap, {{"dead", dead},
                                         {"survivor", survivor},
                                         {"moved", moved},
                                         {"allocated", allocated}}));
    }

    {
        lang::gc::Heap heap;
        const auto dead = heap.allocate_pair(lang::Value::int64(90), lang::Value::int64(91));
        const auto survivor = heap.allocate_pair(lang::Value::int64(92), lang::Value::int64(93));
        auto handle = heap.make_handle(survivor);
        lang::gc::StressConfig stress;
        stress.collect_after_every_allocation = true;
        heap.set_stress_config(stress);

        const auto allocated = heap.allocate_pair(lang::Value::int64(94), lang::Value::int64(95));
        const auto moved = handle.object();
        require(moved != survivor && slot_of(moved) == slot_of(dead),
                "after-every-allocation stress did not trace handle roots\n" +
                    describe_heap(heap, {{"dead", dead},
                                         {"survivor", survivor},
                                         {"moved", moved},
                                         {"allocated", allocated}}));
    }

    {
        lang::VM vm;
        const auto dead = vm.heap().allocate_pair(lang::Value::int64(100), lang::Value::int64(101));
        const auto survivor = vm.heap().allocate_pair(lang::Value::int64(102), lang::Value::int64(103));
        auto handle = vm.heap().make_handle(survivor);
        lang::gc::StressConfig stress;
        stress.collect_every_n_instructions = 1;
        vm.set_gc_stress(stress);

        const auto result =
            test_support::execute_verified(vm, int_result_program(7), "major handle stress");
        require(result.as_i64() == 7, "major instruction stress program returned wrong value");
        const auto moved = handle.object();
        require(moved != survivor && slot_of(moved) == slot_of(dead),
                "major every-instruction stress did not trace handle roots\n" +
                    describe_heap(vm.heap(), {{"dead", dead},
                                              {"survivor", survivor},
                                              {"moved", moved}}));
    }

    {
        lang::VM vm;
        auto old = vm.heap().allocate_pair(lang::Value::int64(110), lang::Value::int64(111));
        auto old_handle = vm.heap().make_handle(old);
        vm.heap().collect_minor();
        old = old_handle.object();
        const auto dead_young = vm.heap().allocate_pair(lang::Value::int64(112), lang::Value::int64(113));
        const auto young = vm.heap().allocate_pair(lang::Value::int64(114), lang::Value::int64(115));
        auto young_handle = vm.heap().make_handle(young);
        lang::gc::StressConfig stress;
        stress.collect_minor_every_n_instructions = 1;
        vm.set_gc_stress(stress);

        const auto result =
            test_support::execute_verified(vm, int_result_program(8), "minor handle stress");
        require(result.as_i64() == 8, "minor instruction stress program returned wrong value");
        const auto moved_young = young_handle.object();
        require(moved_young != young && slot_of(moved_young) == slot_of(dead_young),
                "minor every-instruction stress did not trace handle roots\n" +
                    describe_heap(vm.heap(), {{"old", old},
                                              {"dead_young", dead_young},
                                              {"young", young},
                                              {"moved_young", moved_young}}));
    }

    {
        lang::gc::Heap heap;
        auto old = heap.allocate_pair(lang::Value::int64(120), lang::Value::int64(121));
        auto old_handle = heap.make_handle(old);
        heap.collect_minor();
        old = old_handle.object();
        const auto dead_young = heap.allocate_pair(lang::Value::int64(122), lang::Value::int64(123));
        const auto young = heap.allocate_pair(lang::Value::int64(124), lang::Value::int64(125));
        auto young_handle = heap.make_handle(young);
        lang::gc::StressConfig stress;
        stress.collect_minor_after_every_write_barrier = true;
        heap.set_stress_config(stress);

        heap.set_left(old_handle.object(), lang::Value::object(young_handle.object()));

        const auto moved_young = young_handle.object();
        require(moved_young != young && slot_of(moved_young) == slot_of(dead_young),
                "after-every-barrier stress did not trace handle roots\n" +
                    describe_heap(heap, {{"old", old},
                                         {"dead_young", dead_young},
                                         {"young", young},
                                         {"moved_young", moved_young}}));
        require(heap.TEST_ONLY_remembered_set_size() == 0,
                "handle roots should not become remembered-set entries\n" +
                    describe_heap(heap, {{"old", old}, {"moved_young", moved_young}}));
    }
}

void multiple_handles_to_same_object_rewrite_together() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(130), lang::Value::int64(131));
    const auto survivor = heap.allocate_pair(lang::Value::int64(132), lang::Value::int64(133));

    {
        auto first = heap.make_handle(survivor);
        {
            auto second = heap.make_handle(survivor);
            heap.collect();
            const auto moved = first.object();
            require(moved != survivor && slot_of(moved) == slot_of(dead),
                    "first handle was not rewritten after movement\n" +
                        describe_heap(heap, {{"dead", dead},
                                             {"survivor", survivor},
                                             {"moved", moved}}));
            require(second.object() == moved,
                    "multiple handles to the same object diverged after movement\n" +
                        describe_heap(heap, {{"moved", moved}}));
        }

        require(heap.TEST_ONLY_handle_root_count() == 1,
                "destroying one of two handles left the wrong root count\n" +
                    describe_heap(heap, {{"first", first.object()}}));
        heap.collect();
        require(heap.live_count() == 1,
                "remaining handle did not keep the shared object live\n" +
                    describe_heap(heap, {{"first", first.object()}}));
    }

    heap.collect();
    require(heap.live_count() == 0,
            "shared object stayed live after all handles were destroyed\n" +
                describe_heap(heap, {{"survivor", survivor}}));
}

void moved_handles_transfer_registration_without_copying_roots() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(140), lang::Value::int64(141));
    const auto survivor = heap.allocate_pair(lang::Value::int64(142), lang::Value::int64(143));
    const auto replaced = heap.allocate_pair(lang::Value::int64(144), lang::Value::int64(145));

    auto source = heap.make_handle(survivor);
    auto moved = std::move(source);
    require_throws([&] { (void)source.object(); },
                   "moved-from handle remained usable after move construction");

    auto target = heap.make_handle(replaced);
    target = std::move(moved);
    require_throws([&] { (void)moved.object(); },
                   "moved-from handle remained usable after move assignment");

    heap.collect();

    const auto moved_survivor = target.object();
    require(moved_survivor != survivor && slot_of(moved_survivor) == slot_of(dead),
            "move-assigned handle did not root the transferred object\n" +
                describe_heap(heap, {{"dead", dead},
                                     {"survivor", survivor},
                                     {"replaced", replaced},
                                     {"moved_survivor", moved_survivor}}));
    require(heap.live_count() == 1,
            "move assignment kept the replaced handle's old object rooted\n" +
                describe_heap(heap, {{"moved_survivor", moved_survivor},
                                     {"replaced", replaced}}));
}

void handle_to_cyclic_structure_rewrites_cycle_fields() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(150), lang::Value::int64(151));
    const auto first = heap.allocate_pair(lang::Value::int64(152), lang::Value::int64(153));
    const auto second = heap.allocate_pair(lang::Value::int64(154), lang::Value::int64(155));
    heap.set_left(first, lang::Value::object(second));
    heap.set_right(second, lang::Value::object(first));
    auto handle = heap.make_handle(first);

    heap.collect();

    const auto moved_first = handle.object();
    const auto moved_second = heap.left(moved_first).as_object();
    require(moved_first != first && slot_of(moved_first) == slot_of(dead),
            "cyclic structure root handle was not rewritten after compaction\n" +
                describe_heap(heap, {{"dead", dead},
                                     {"first", first},
                                     {"moved_first", moved_first}}));
    require(heap.right(moved_second).as_object() == moved_first,
            "cycle back-edge was not rewritten through the handle-rooted collection\n" +
                describe_heap(heap, {{"moved_first", moved_first},
                                     {"moved_second", moved_second}}));
    require(heap.live_count() == 2,
            "handle-rooted cycle should keep exactly two objects live\n" +
                describe_heap(heap, {{"moved_first", moved_first},
                                     {"moved_second", moved_second}}));
}

void constructing_handle_from_invalid_object_id_traps() {
    lang::gc::Heap heap;
    const auto invalid = static_cast<lang::ObjectId>(1) << kGenerationShift;

    require_throws([&] { (void)heap.make_handle(invalid); },
                   "make_handle(ObjectId) accepted an invalid object id");
    require_throws([&] { (void)heap.make_handle(lang::Value::object(invalid)); },
                   "make_handle(Value::object) accepted an invalid object id");
    require(heap.TEST_ONLY_handle_root_count() == 0,
            "failed handle construction registered a dangling root slot\n" +
                describe_heap(heap, {{"invalid", invalid}}));
}

void heap_teardown_with_live_handle_traps_in_child() {
#if defined(__unix__) || defined(__APPLE__)
    const auto pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed for heap teardown trap test");
    }
    if (pid == 0) {
        auto* heap = new lang::gc::Heap();
        const auto object = heap->allocate_pair(lang::Value::int64(160),
                                                lang::Value::int64(161));
        auto handle = heap->make_handle(object);
        (void)handle.object();
        delete heap;
        std::_Exit(0);
    }

    int status = 0;
    const auto waited = waitpid(pid, &status, 0);
    require(waited == pid, "waitpid failed for heap teardown trap test");
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
            "destroying a heap with live handles did not trap with SIGABRT");
#else
    throw std::runtime_error(
        "heap teardown trap test requires POSIX fork/wait process isolation");
#endif
}

struct TestCase {
    const char* name;
    const char* invariant;
    const char* baseline_red;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"handle_keeps_object_alive_and_rewrites_after_major_collection",
         "handle slots are traced and rewritten during major collection",
         "RED before implementation: Heap::make_handle and lang::gc::Handle do not exist",
         handle_keeps_object_alive_and_rewrites_after_major_collection},
        {"handle_keeps_young_object_alive_and_rewrites_after_minor_collection",
         "handle slots are traced and rewritten during minor collection",
         "RED before implementation: handles are not part of the minor root set",
         handle_keeps_young_object_alive_and_rewrites_after_minor_collection},
        {"destroyed_handle_no_longer_roots_its_slot",
         "handle destruction deregisters the mutable root slot before the next collection",
         "RED before implementation: there is no handle registration table to deregister",
         destroyed_handle_no_longer_roots_its_slot},
        {"stale_raw_id_traps_while_handle_stays_valid_after_move",
         "external raw ObjectId copies remain stale while handles are movement-safe roots",
         "RED before implementation: external C++ roots require hand-written RootProvider slots",
         stale_raw_id_traps_while_handle_stays_valid_after_move},
        {"handles_survive_heap_and_vm_stress_modes",
         "all deterministic GC stress collection paths include handle slots as roots",
         "RED before implementation: stress collection roots do not include handles",
         handles_survive_heap_and_vm_stress_modes},
        {"multiple_handles_to_same_object_rewrite_together",
         "duplicate handles to one object are independent root slots rewritten to the same id",
         "RED before implementation: there is no deterministic multi-handle registration order",
         multiple_handles_to_same_object_rewrite_together},
        {"moved_handles_transfer_registration_without_copying_roots",
         "move construction/assignment transfer one root registration and invalidate source handles",
         "RED before implementation: Handle move semantics are undefined",
         moved_handles_transfer_registration_without_copying_roots},
        {"handle_to_cyclic_structure_rewrites_cycle_fields",
         "handle-rooted cyclic graphs keep all reachable objects and rewrite cycle fields",
         "RED before implementation: handle roots are not traced into the object graph",
         handle_to_cyclic_structure_rewrites_cycle_fields},
        {"constructing_handle_from_invalid_object_id_traps",
         "invalid ObjectIds are rejected before registering handle slots",
         "RED before implementation: make_handle validation API does not exist",
         constructing_handle_from_invalid_object_id_traps},
        {"heap_teardown_with_live_handle_traps_in_child",
         "heap/handle lifetime violations trap deterministically under asserts",
         "RED before implementation: heap teardown has no live-handle assertion",
         heap_teardown_with_live_handle_traps_in_child},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << " | invariant: " << test.invariant
                      << " | baseline: " << test.baseline_red << "\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << " | invariant: " << test.invariant
                      << " | baseline: " << test.baseline_red << "\n"
                      << e.what() << "\n";
        }
    }

    if (failures != 0) {
        std::cerr << failures << " iteration-12 handle test(s) failed\n";
        return 1;
    }
    return 0;
}
