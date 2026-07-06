#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr lang::ObjectId kSlotMask = 0xFFFF'FFFFull;

std::uint32_t slot_of(lang::ObjectId id) {
    return static_cast<std::uint32_t>(id & kSlotMask);
}

const char* op_name(lang::OpCode op) {
    switch (op) {
    case lang::OpCode::ConstantI64:
        return "ConstantI64";
    case lang::OpCode::AddI64:
        return "AddI64";
    case lang::OpCode::LessI64:
        return "LessI64";
    case lang::OpCode::AllocPair:
        return "AllocPair";
    case lang::OpCode::GetLeft:
        return "GetLeft";
    case lang::OpCode::GetRight:
        return "GetRight";
    case lang::OpCode::SetLeft:
        return "SetLeft";
    case lang::OpCode::SetRight:
        return "SetRight";
    case lang::OpCode::LoadLocal:
        return "LoadLocal";
    case lang::OpCode::StoreLocal:
        return "StoreLocal";
    case lang::OpCode::Jump:
        return "Jump";
    case lang::OpCode::JumpIfFalse:
        return "JumpIfFalse";
    case lang::OpCode::Collect:
        return "Collect";
    case lang::OpCode::Return:
        return "Return";
    }
    return "<unknown>";
}

std::string describe(const lang::Function& function, std::size_t focus_pc) {
    std::ostringstream out;
    out << "locals=" << function.local_count << " focus_pc=" << focus_pc << "\n";
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        const auto& ins = function.code[pc];
        out << "  #" << pc << " " << op_name(ins.op) << " " << ins.operand;
        if (pc == focus_pc) {
            out << "  <--";
        }
        out << "\n";
    }
    return out.str();
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
        << " remembered_set_size=" << heap.TEST_ONLY_remembered_set_size() << "\n";
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

struct VectorRoots final : lang::gc::RootProvider {
    std::vector<lang::Value> roots;

    void trace_roots(lang::gc::RootVisitor& visitor) override {
        for (auto& root : roots) {
            visitor.visit(root);
        }
    }
};

lang::ObjectId promote_single_root(lang::gc::Heap& heap, VectorRoots& roots,
                                   lang::ObjectId id) {
    roots.roots.clear();
    roots.roots.push_back(lang::Value::object(id));
    heap.collect_minor(roots);
    return roots.roots.at(0).as_object();
}

void old_to_young_barrier_keeps_young_alive_through_remembered_set_only() {
    lang::gc::Heap heap;
    VectorRoots roots;

    auto old = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    old = promote_single_root(heap, roots, old);
    require(heap.TEST_ONLY_is_old_object(old),
            "setup failed to promote old object\n" + describe_heap(heap, {{"old", old}}));

    const auto young = heap.allocate_pair(lang::Value::int64(10), lang::Value::int64(11));
    require(heap.TEST_ONLY_is_young_object(young),
            "new allocation was not young\n" + describe_heap(heap, {{"young", young}}));
    heap.set_left(old, lang::Value::object(young));
    require(heap.TEST_ONLY_remembered_set_size() == 1,
            "old-to-young store did not record the old object before collection\n" +
                describe_heap(heap, {{"old", old}, {"young", young}}));

    roots.roots = {lang::Value::object(old)};
    heap.collect_minor(roots);

    old = roots.roots.at(0).as_object();
    const auto kept = heap.left(old).as_object();
    require(heap.live_count() == 2,
            "minor collection swept young object reachable only from remembered old field\n" +
                describe_heap(heap, {{"old", old}, {"kept", kept}, {"pre_minor_young", young}}));
    require(heap.object(kept).left.as_i64() == 10,
            "remembered young object survived with wrong payload\n" +
                describe_heap(heap, {{"old", old}, {"kept", kept}}));
    require(heap.TEST_ONLY_is_old_object(kept),
            "young survivor was not promoted after one minor collection\n" +
                describe_heap(heap, {{"kept", kept}}));
}

void promoted_object_survives_minor_without_roots_until_major() {
    lang::gc::Heap heap;
    VectorRoots roots;

    auto promoted = heap.allocate_pair(lang::Value::int64(20), lang::Value::int64(21));
    promoted = promote_single_root(heap, roots, promoted);
    require(heap.TEST_ONLY_is_old_object(promoted),
            "rooted survivor was not promoted\n" + describe_heap(heap, {{"promoted", promoted}}));

    roots.roots.clear();
    heap.collect_minor(roots);
    require(heap.live_count() == 1,
            "minor collection should not sweep an old object even when it is not retraced\n" +
                describe_heap(heap, {{"promoted", promoted}}));

    heap.collect(roots);
    require(heap.live_count() == 0,
            "major collection failed to sweep promoted object after roots were dropped\n" +
                describe_heap(heap, {{"promoted", promoted}}));
}

void remembered_set_does_not_keep_dead_old_graph_alive_forever() {
    lang::gc::Heap heap;
    VectorRoots roots;

    auto old = heap.allocate_pair(lang::Value::int64(30), lang::Value::int64(31));
    old = promote_single_root(heap, roots, old);
    const auto young = heap.allocate_pair(lang::Value::int64(32), lang::Value::int64(33));
    heap.set_right(old, lang::Value::object(young));
    require(heap.TEST_ONLY_remembered_set_size() == 1,
            "setup did not create a remembered old-to-young edge\n" +
                describe_heap(heap, {{"old", old}, {"young", young}}));

    roots.roots.clear();
    heap.collect_minor(roots);
    require(heap.live_count() == 2,
            "minor collection should conservatively retain young through remembered old entry\n" +
                describe_heap(heap, {{"old", old}, {"young", young}}));

    heap.collect(roots);
    require(heap.live_count() == 0 && heap.TEST_ONLY_remembered_set_size() == 0,
            "major collection or remembered-set pruning leaked a dead old-to-young graph\n" +
                describe_heap(heap, {{"old", old}, {"young", young}}));
}

void barrier_elision_debug_api_proves_validator_is_not_vacuous() {
    lang::gc::Heap heap;
    VectorRoots roots;

    auto old = heap.allocate_pair(lang::Value::int64(40), lang::Value::int64(41));
    old = promote_single_root(heap, roots, old);
    const auto young = heap.allocate_pair(lang::Value::int64(42), lang::Value::int64(43));

    heap.TEST_ONLY_skip_next_write_barrier_for_barrier_validator();
    heap.set_left(old, lang::Value::object(young));

    require_throws([&] { heap.TEST_ONLY_validate_gc_invariants(); },
                   "validator did not trap a deliberate missing old-to-young barrier\n" +
                       describe_heap(heap, {{"old", old}, {"young", young}}));
}

void minor_collection_rewrites_moved_young_in_old_field() {
    lang::gc::Heap heap;
    VectorRoots roots;

    auto old = heap.allocate_pair(lang::Value::int64(50), lang::Value::int64(51));
    old = promote_single_root(heap, roots, old);
    const auto dead_young = heap.allocate_pair(lang::Value::int64(52), lang::Value::int64(53));
    const auto young = heap.allocate_pair(lang::Value::int64(54), lang::Value::int64(55));
    heap.set_left(old, lang::Value::object(young));

    roots.roots = {lang::Value::object(old)};
    heap.collect_minor(roots);
    old = roots.roots.at(0).as_object();
    const auto moved_young = heap.left(old).as_object();

    require(moved_young != young && slot_of(moved_young) == slot_of(dead_young),
            "minor collection did not move young survivor into the dead young slot\n" +
                describe_heap(heap, {{"old", old},
                                     {"dead_young", dead_young},
                                     {"young", young},
                                     {"moved_young", moved_young}}));
    require(heap.object(moved_young).left.as_i64() == 54,
            "moved young survivor payload changed during minor collection\n" +
                describe_heap(heap, {{"moved_young", moved_young}}));
    require(heap.TEST_ONLY_is_old_object(moved_young),
            "moved young survivor was not promoted\n" +
                describe_heap(heap, {{"moved_young", moved_young}}));
}

void major_collection_rewrites_remembered_set_entries_before_pruning() {
    lang::gc::Heap heap;
    VectorRoots roots;

    auto dead_old = heap.allocate_pair(lang::Value::int64(60), lang::Value::int64(61));
    auto old = heap.allocate_pair(lang::Value::int64(62), lang::Value::int64(63));
    roots.roots = {lang::Value::object(dead_old), lang::Value::object(old)};
    heap.collect_minor(roots);
    dead_old = roots.roots.at(0).as_object();
    old = roots.roots.at(1).as_object();
    roots.roots = {lang::Value::object(old)};

    const auto young = heap.allocate_pair(lang::Value::int64(64), lang::Value::int64(65));
    heap.set_right(old, lang::Value::object(young));
    require(heap.TEST_ONLY_remembered_set_size() == 1,
            "setup did not record remembered old object\n" +
                describe_heap(heap, {{"dead_old", dead_old}, {"old", old}, {"young", young}}));

    heap.collect(roots);
    old = roots.roots.at(0).as_object();
    const auto promoted_child = heap.right(old).as_object();

    require(slot_of(old) == 0,
            "major collection did not move remembered old object below dead old slot\n" +
                describe_heap(heap, {{"dead_old", dead_old}, {"old", old}}));
    require(heap.live_count() == 2 && heap.TEST_ONLY_remembered_set_size() == 0,
            "major collection did not prune rewritten remembered set after promotion\n" +
                describe_heap(heap, {{"old", old}, {"promoted_child", promoted_child}}));
    require(heap.object(promoted_child).left.as_i64() == 64,
            "remembered child payload changed during major movement\n" +
                describe_heap(heap, {{"promoted_child", promoted_child}}));
}

lang::Function old_mutation_loop_program() {
    lang::Function function;
    function.local_count = 2;
    function.code = {
        {lang::OpCode::ConstantI64, -1}, // 0 old.left
        {lang::OpCode::ConstantI64, -2}, // 1 old.right
        {lang::OpCode::AllocPair, 0},    // 2
        {lang::OpCode::StoreLocal, 0},   // 3 old candidate
        {lang::OpCode::Collect, 0},      // 4 promote old through major collection
        {lang::OpCode::ConstantI64, 0},  // 5 i
        {lang::OpCode::StoreLocal, 1},   // 6
        {lang::OpCode::LoadLocal, 1},    // 7 loop header
        {lang::OpCode::ConstantI64, 5},  // 8
        {lang::OpCode::LessI64, 0},      // 9
        {lang::OpCode::JumpIfFalse, 24}, // 10
        {lang::OpCode::LoadLocal, 1},    // 11 child.left = i
        {lang::OpCode::ConstantI64, 99}, // 12 child.right
        {lang::OpCode::AllocPair, 0},    // 13 child
        {lang::OpCode::StoreLocal, 1},   // 14 stash child in i local temporarily
        {lang::OpCode::LoadLocal, 0},    // 15 old receiver
        {lang::OpCode::LoadLocal, 1},    // 16 child value
        {lang::OpCode::SetLeft, 0},      // 17 barrier-triggering old->young store
        {lang::OpCode::LoadLocal, 1},    // 18 i = child.left + 1
        {lang::OpCode::GetLeft, 0},      // 19
        {lang::OpCode::ConstantI64, 1},  // 20
        {lang::OpCode::AddI64, 0},       // 21
        {lang::OpCode::StoreLocal, 1},   // 22
        {lang::OpCode::Jump, 7},         // 23
        {lang::OpCode::LoadLocal, 0},    // 24
        {lang::OpCode::Return, 0},       // 25
    };
    return function;
}

void bytecode_loop_mutates_old_object_under_after_every_barrier_stress() {
    const auto function = old_mutation_loop_program();
    require(lang::verify(function),
            "verifier rejected old-object mutation loop\n" + describe(function, 17));

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_minor_after_every_write_barrier = true;
    stress.collect_minor_every_n_instructions = 3;
    vm.set_gc_stress(stress);

    const auto result = vm.execute(function);
    require(vm.heap().TEST_ONLY_validation_count() >= 6,
            "after-every-barrier/minor-every-N stress did not run collection validators\n" +
                describe(function, 17));
    require(result.is_object(), "old-object mutation loop returned non-object\n" +
                                    describe(function, 25));
    const auto child = vm.heap().left(result.as_object()).as_object();
    require(vm.heap().object(child).left.as_i64() == 4,
            "after-every-barrier stress lost the last young object stored into old field\n" +
                describe(function, 17) +
                describe_heap(vm.heap(), {{"result", result.as_object()}, {"child", child}}));
    require(vm.heap().TEST_ONLY_remembered_set_size() == 0,
            "barrier stress left stale remembered-set entries after promotion/pruning\n" +
                describe(function, 17) +
                describe_heap(vm.heap(), {{"result", result.as_object()}, {"child", child}}));
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
        {"old_to_young_barrier_keeps_young_alive_through_remembered_set_only",
         "old-to-young write barrier; no reachable young swept by minor collection",
         "RED before implementation: no minor collector or remembered-set barrier API",
         old_to_young_barrier_keeps_young_alive_through_remembered_set_only},
        {"promoted_object_survives_minor_without_roots_until_major",
         "survive-one-collection promotion; minor traces young only, major sweeps all garbage",
         "RED before implementation: no object generation state or minor collection",
         promoted_object_survives_minor_without_roots_until_major},
        {"remembered_set_does_not_keep_dead_old_graph_alive_forever",
         "remembered-set pruning; major collection does not treat remset as a permanent root",
         "RED before implementation: no remembered-set pruning",
         remembered_set_does_not_keep_dead_old_graph_alive_forever},
        {"barrier_elision_debug_api_proves_validator_is_not_vacuous",
         "barrier-completeness validator catches an unremembered old-to-young edge",
         "RED before implementation: no deliberate barrier-elision debug hook",
         barrier_elision_debug_api_proves_validator_is_not_vacuous},
        {"minor_collection_rewrites_moved_young_in_old_field",
         "minor moving collection rewrites old-object fields before mutator resumes",
         "RED before implementation: no minor forwarding/rewrite pass",
         minor_collection_rewrites_moved_young_in_old_field},
        {"major_collection_rewrites_remembered_set_entries_before_pruning",
         "remembered-set entries are valid across movement and then pruned deterministically",
         "RED before implementation: remembered set does not exist",
         major_collection_rewrites_remembered_set_entries_before_pruning},
        {"bytecode_loop_mutates_old_object_under_after_every_barrier_stress",
         "bytecode stores cannot bypass Heap::store_pair_field barrier under deterministic stress",
         "RED before implementation: no after-every-barrier or minor-every-N stress modes",
         bytecode_loop_mutates_old_object_under_after_every_barrier_stress},
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
        std::cerr << failures << " iteration-4 generational test(s) failed\n";
        return 1;
    }
    return 0;
}
