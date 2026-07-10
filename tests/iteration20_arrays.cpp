#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
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
    case lang::OpCode::AllocArray:
        return "AllocArray";
    case lang::OpCode::ArrayGet:
        return "ArrayGet";
    case lang::OpCode::ArraySet:
        return "ArraySet";
    case lang::OpCode::ArrayLen:
        return "ArrayLen";
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
    case lang::OpCode::Call:
        return "Call";
    case lang::OpCode::Return:
        return "Return";
    case lang::OpCode::Nil:
        return "Nil";
    case lang::OpCode::IsNil:
        return "IsNil";
    case lang::OpCode::AllocRefArray:
        return "AllocRefArray";
    case lang::OpCode::RefArrayGet:
        return "RefArrayGet";
    case lang::OpCode::RefArraySet:
        return "RefArraySet";
    }
    return "<unknown>";
}

std::string describe(const lang::Function& function, std::size_t focus_pc = 0) {
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

void set_signature(lang::Function& function,
                   std::vector<lang::ValueKind> parameters,
                   lang::ValueKind return_type) {
    function.signature.parameters = std::move(parameters);
    function.signature.return_type = return_type;
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

void scalar_array_payload_that_looks_like_dead_object_id_is_not_traced() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(10), lang::Value::int64(11));
    const auto raw_dead_id = static_cast<std::int64_t>(dead);
    const auto array = heap.allocate_scalar_array(3, -1);
    heap.array_set(array, 1, raw_dead_id);

    VectorRoots roots;
    roots.roots = {lang::Value::object(array)};
    heap.collect(roots);

    const auto moved_array = roots.roots.at(0).as_object();
    require(heap.live_count() == 1,
            "scalar array raw payload was treated as a reference and kept a dead pair live");
    require(heap.array_length(moved_array) == 3,
            "scalar array length changed during compaction");
    require(heap.array_get(moved_array, 1) == raw_dead_id,
            "scalar array raw payload bit-pattern was not byte-preserved");
    require_throws([&] { (void)heap.object(dead); },
                   "dead object id remained valid after collection even though only a raw i64 matched it");
}

void non_uniform_compaction_advances_by_descriptor_size_and_rewrites_fields() {
    lang::gc::Heap heap;
    const auto dead_array = heap.allocate_scalar_array(4, 100);
    const auto pair = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    const auto array = heap.allocate_scalar_array(3, 7);
    const auto tail = heap.allocate_pair(lang::Value::int64(9), lang::Value::int64(10));
    heap.set_left(pair, lang::Value::object(array));
    heap.set_right(pair, lang::Value::object(tail));

    VectorRoots roots;
    roots.roots = {lang::Value::object(pair)};
    heap.collect(roots);

    const auto moved_pair = roots.roots.at(0).as_object();
    const auto moved_array = heap.left(moved_pair).as_object();
    const auto moved_tail = heap.right(moved_pair).as_object();

    require(slot_of(moved_pair) == 0,
            "root pair did not slide into the first compacted base slot");
    require(slot_of(moved_array) == 1,
            "array did not slide immediately after the pair base slot");
    require(slot_of(moved_tail) == 4,
            "tail pair was not placed after the three-slot scalar array payload");
    require(heap.array_length(moved_array) == 3 && heap.array_get(moved_array, 2) == 7,
            "moved scalar array descriptor or payload changed");
    require(heap.object(moved_tail).left.as_i64() == 9,
            "tail pair payload changed while compacting across " +
                std::to_string(slot_of(dead_array)) + " dead array slots");
}

void minor_collection_remembers_old_pair_to_young_array_reference() {
    lang::gc::Heap heap;
    VectorRoots roots;

    auto old = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    roots.roots = {lang::Value::object(old)};
    heap.collect_minor(roots);
    old = roots.roots.at(0).as_object();
    require(heap.TEST_ONLY_is_old_object(old), "setup did not promote the root pair");

    const auto dead_young = heap.allocate_pair(lang::Value::int64(30), lang::Value::int64(31));
    const auto array = heap.allocate_scalar_array(2, 44);
    heap.set_left(old, lang::Value::object(array));
    require(heap.TEST_ONLY_remembered_set_size() == 1,
            "old-to-young array store did not record the old pair");

    roots.roots = {lang::Value::object(old)};
    heap.collect_minor(roots);
    old = roots.roots.at(0).as_object();
    const auto moved_array = heap.left(old).as_object();

    require(slot_of(moved_array) == slot_of(dead_young),
            "minor collection did not move the young array into the dead young pair slot");
    require(heap.TEST_ONLY_is_old_object(moved_array),
            "young scalar array survivor was not promoted");
    require(heap.array_get(moved_array, 0) == 44 && heap.array_get(moved_array, 1) == 44,
            "remembered scalar array payload changed during minor collection");
}

lang::Function array_bytecode_program() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Int64);
    function.local_count = 1;
    function.code = {
        {lang::OpCode::ConstantI64, 3},
        {lang::OpCode::ConstantI64, 7},
        {lang::OpCode::AllocArray, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::ArraySet, 0},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ArrayLen, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ArrayGet, 0},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };
    return function;
}

void bytecode_arrays_execute_under_stack_map_checked_gc_stress() {
    const auto function = array_bytecode_program();
    auto report = lang::verify_with_diagnostics(function);
    require(report.result.has_value(),
            "verifier rejected valid array bytecode\n" + describe(function, 2));
    require(report.result->stack_maps.at(3).object_slots == std::vector<bool>{true},
            "array reference was not emitted as an object stack-map slot before StoreLocal");

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 2;
    stress.collect_minor_every_n_instructions = 3;
    vm.set_gc_stress(stress);
    const auto result = test_support::execute_verified(vm, function, describe(function, 7));
    require(result.as_i64() == 45,
            "array bytecode returned the wrong len+element result under GC stress");
}

void array_get_out_of_bounds_traps_deterministically() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Int64);
    function.local_count = 0;
    function.code = {
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocArray, 0},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::ArrayGet, 0},
        {lang::OpCode::Return, 0},
    };
    lang::VM vm;
    require_throws([&] { (void)test_support::execute_verified(vm, function, describe(function, 4)); },
                   "ArrayGet index equal to length did not trap deterministically");
}

void verifier_rejects_array_get_on_pair_with_structured_reason() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Int64);
    function.local_count = 0;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ArrayGet, 0},
        {lang::OpCode::Return, 0},
    };

    auto report = lang::verify_with_diagnostics(function);
    require(!report.result.has_value(), "verifier accepted ArrayGet on a pair receiver");
    require(!report.diagnostics.empty(), "verifier rejected malformed array bytecode without diagnostics");
    require(report.diagnostics.front().reason == lang::VerifierReason::BadArrayOperation,
            "malformed array bytecode used the wrong structured verifier reason: " +
                lang::format_verifier_diagnostic(report.diagnostics.front()));
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
        {"scalar_array_payload_that_looks_like_dead_object_id_is_not_traced",
         "descriptor-driven precise scan: ScalarArray has zero reference payload slots",
         "RED before implementation: heap has only fixed two-Value pair objects",
         scalar_array_payload_that_looks_like_dead_object_id_is_not_traced},
        {"non_uniform_compaction_advances_by_descriptor_size_and_rewrites_fields",
         "moving compaction advances the free cursor by each object's descriptor size",
         "RED before implementation: compaction assumes one uniform pair storage slot",
         non_uniform_compaction_advances_by_descriptor_size_and_rewrites_fields},
        {"minor_collection_remembers_old_pair_to_young_array_reference",
         "write barrier and remembered-set validators handle array referents",
         "RED before implementation: arrays cannot be allocated or remembered",
         minor_collection_remembers_old_pair_to_young_array_reference},
        {"bytecode_arrays_execute_under_stack_map_checked_gc_stress",
         "array refs are precise stack-map object slots and raw payload stays in heap",
         "RED before implementation: array opcodes and verifier kind do not exist",
         bytecode_arrays_execute_under_stack_map_checked_gc_stress},
        {"array_get_out_of_bounds_traps_deterministically",
         "ArrayGet performs deterministic runtime bounds checks",
         "RED before implementation: VM has no ArrayGet trap path",
         array_get_out_of_bounds_traps_deterministically},
        {"verifier_rejects_array_get_on_pair_with_structured_reason",
         "verifier rejects malformed array bytecode with stable diagnostics",
         "RED before implementation: no BadArrayOperation verifier reason",
         verifier_rejects_array_get_on_pair_with_structured_reason},
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
        std::cerr << failures << " iteration-20 array test(s) failed\n";
        return 1;
    }
    return 0;
}
