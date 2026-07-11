#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"
#include "test_support.hpp"

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
    case lang::OpCode::AllocArray:
        return "AllocArray";
    case lang::OpCode::ArrayGet:
        return "ArrayGet";
    case lang::OpCode::ArraySet:
        return "ArraySet";
    case lang::OpCode::ArrayLen:
        return "ArrayLen";
    case lang::OpCode::AllocRefArray:
        return "AllocRefArray";
    case lang::OpCode::RefArrayGet:
        return "RefArrayGet";
    case lang::OpCode::RefArraySet:
        return "RefArraySet";
    case lang::OpCode::PushStr:
        return "PushStr";
    case lang::OpCode::StrLen:
        return "StrLen";
    case lang::OpCode::StrEq:
        return "StrEq";
    case lang::OpCode::StrConcat:
        return "StrConcat";
    case lang::OpCode::StrIndex:
        return "StrIndex";
    case lang::OpCode::AllocClosure:
        return "AllocClosure";
    case lang::OpCode::CallClosure:
        return "CallClosure";
    case lang::OpCode::LoadCapture:
        return "LoadCapture";
    case lang::OpCode::AllocMap:
        return "AllocMap";
    case lang::OpCode::MapSet:
        return "MapSet";
    case lang::OpCode::MapGet:
        return "MapGet";
    case lang::OpCode::MapHas:
        return "MapHas";
    case lang::OpCode::MapLen:
        return "MapLen";
    case lang::OpCode::AllocWeak:
        return "AllocWeak";
    case lang::OpCode::WeakGet:
        return "WeakGet";
    case lang::OpCode::MapKeyAt:
        return "MapKeyAt";
    case lang::OpCode::MapValueAt:
        return "MapValueAt";
    case lang::OpCode::Print:
        return "Print";
    case lang::OpCode::I64ToStr:
        return "I64ToStr";
    case lang::OpCode::StrToI64:
        return "StrToI64";
    case lang::OpCode::BoolToStr:
        return "BoolToStr";
    case lang::OpCode::StrSub:
        return "StrSub";
    case lang::OpCode::StrLt:
        return "StrLt";
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

void reachability_through_ref_array_element_survives_and_forwards() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(-1), lang::Value::int64(-2));
    const auto target = heap.allocate_pair(lang::Value::int64(10), lang::Value::int64(11));
    const auto array = heap.allocate_ref_array(2, lang::Value::object(target));

    VectorRoots roots;
    roots.roots = {lang::Value::object(array)};
    heap.collect(roots);

    const auto moved_array = roots.roots.at(0).as_object();
    const auto moved_target = heap.ref_array_get(moved_array, 0).as_object();
    require(heap.live_count() == 2,
            "object reachable only through RefArray element was not the only retained payload");
    require(heap.ref_array_get(moved_array, 1).as_object() == moved_target,
            "all RefArray element slots were not forwarded consistently");
    require(heap.left(moved_target).as_i64() == 10 &&
                heap.right(moved_target).as_i64() == 11,
            "forwarded RefArray element does not name the original pair payload");
    require(slot_of(moved_target) == slot_of(dead),
            "reachable element did not slide into the dead object's storage slot");
    require_throws([&] { (void)heap.object(target); },
                   "old pre-compaction element id remained valid after movement");
}

void old_ref_array_to_young_element_store_is_remembered_for_minor_collection() {
    lang::gc::Heap heap;
    VectorRoots roots;

    auto initial = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    auto array = heap.allocate_ref_array(1, lang::Value::object(initial));
    roots.roots = {lang::Value::object(array)};
    heap.collect_minor(roots);
    array = roots.roots.at(0).as_object();
    initial = heap.ref_array_get(array, 0).as_object();
    require(heap.TEST_ONLY_is_old_object(array), "setup did not promote RefArray owner");
    require(heap.TEST_ONLY_is_old_object(initial), "setup did not promote initial element");

    const auto dead_young = heap.allocate_pair(lang::Value::int64(30), lang::Value::int64(31));
    const auto young = heap.allocate_pair(lang::Value::int64(40), lang::Value::int64(41));
    heap.ref_array_set(array, 0, lang::Value::object(young));
    require(heap.TEST_ONLY_remembered_set_size() == 1,
            "old-to-young RefArray element store did not record the old owner");

    heap.collect_minor(roots);
    array = roots.roots.at(0).as_object();
    const auto moved_young = heap.ref_array_get(array, 0).as_object();

    require(slot_of(moved_young) == slot_of(dead_young),
            "minor collection did not move young RefArray element into the dead slot");
    require(heap.TEST_ONLY_is_old_object(moved_young),
            "young RefArray element survivor was not promoted");
    require(heap.left(moved_young).as_i64() == 40 &&
                heap.right(moved_young).as_i64() == 41,
            "remembered RefArray element was not retained through minor collection");
}

void mixed_heap_pairs_scalar_arrays_and_ref_arrays_compact_together() {
    lang::gc::Heap heap;
    const auto dead_array = heap.allocate_scalar_array(4, 100);
    const auto scalar = heap.allocate_scalar_array(3, 7);
    const auto leaf = heap.allocate_pair(lang::Value::int64(5), lang::Value::int64(6));
    const auto refs = heap.allocate_ref_array(2, lang::Value::object(leaf));
    const auto anchor = heap.allocate_pair(lang::Value::object(refs), lang::Value::object(scalar));

    VectorRoots roots;
    roots.roots = {lang::Value::object(anchor)};
    heap.collect(roots);

    const auto moved_anchor = roots.roots.at(0).as_object();
    const auto moved_refs = heap.left(moved_anchor).as_object();
    const auto moved_scalar = heap.right(moved_anchor).as_object();
    const auto moved_leaf = heap.ref_array_get(moved_refs, 0).as_object();

    require(slot_of(moved_scalar) == 0,
            "scalar array did not compact into the first dead storage run");
    require(slot_of(moved_leaf) == 3,
            "pair did not compact immediately after three scalar payload slots");
    require(slot_of(moved_refs) == 4,
            "RefArray did not compact after the pair storage slot");
    require(slot_of(moved_anchor) == 6,
            "anchor pair did not compact after the two-slot RefArray payload");
    require(heap.array_get(moved_scalar, 2) == 7,
            "scalar array payload changed while mixed heap compacted");
    require(heap.ref_array_get(moved_refs, 1).as_object() == moved_leaf,
            "RefArray duplicate element references were not forwarded consistently");
    require(heap.left(moved_leaf).as_i64() == 5,
            "pair payload changed while mixed heap compacted across " +
                std::to_string(slot_of(dead_array)) + " dead array slots");
}

void descriptor_contrast_ref_array_traces_scalar_array_does_not() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(-9), lang::Value::int64(-8));
    const auto target = heap.allocate_pair(lang::Value::int64(70), lang::Value::int64(71));
    const auto raw_target_id = static_cast<std::int64_t>(target);
    const auto scalar = heap.allocate_scalar_array(1, raw_target_id);
    const auto refs = heap.allocate_ref_array(1, lang::Value::object(target));

    VectorRoots roots;
    roots.roots = {lang::Value::object(scalar), lang::Value::object(refs)};
    heap.collect(roots);

    const auto moved_scalar = roots.roots.at(0).as_object();
    const auto moved_refs = roots.roots.at(1).as_object();
    const auto moved_target = heap.ref_array_get(moved_refs, 0).as_object();

    require(heap.live_count() == 3,
            "descriptor contrast retained an unexpected object count");
    require(slot_of(moved_target) == slot_of(dead),
            "RefArray element was not traced and forwarded through movement");
    require(heap.array_get(moved_scalar, 0) == raw_target_id,
            "ScalarArray raw payload bit-pattern was rewritten as if it were a reference");
    require(static_cast<std::int64_t>(moved_target) != raw_target_id,
            "test setup did not force RefArray element forwarding");
}

lang::Function ref_array_bytecode_program() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Object);
    function.local_count = 2;
    function.code = {
        {lang::OpCode::ConstantI64, 13},
        {lang::OpCode::ConstantI64, 14},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::AllocRefArray, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::RefArrayGet, 0},
        {lang::OpCode::Return, 0},
    };
    return function;
}

void bytecode_ref_array_get_returns_precise_reference_slot_under_gc_stress() {
    const auto function = ref_array_bytecode_program();
    auto report = lang::verify_with_diagnostics(function);
    require(report.result.has_value(),
            "verifier rejected valid RefArray bytecode\n" + describe(function, 6));
    require(report.result->stack_maps.at(7).object_slots == std::vector<bool>{true},
            "RefArray allocation was not emitted as a reference stack-map slot");
    require(report.result->stack_maps.at(12).object_slots == std::vector<bool>{true},
            "RefArrayGet result was not emitted as a reference stack-map slot");

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 2;
    stress.collect_minor_every_n_instructions = 3;
    vm.set_gc_stress(stress);
    const auto result = test_support::execute_verified(vm, function, describe(function, 11));
    const auto object = result.as_object();
    require(vm.heap().left(object).as_i64() == 13 &&
                vm.heap().right(object).as_i64() == 14,
            "RefArrayGet returned the wrong object after stack-map checked GC stress");
}

void ref_array_get_out_of_bounds_traps_deterministically() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Object);
    function.local_count = 0;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 5},
        {lang::OpCode::ConstantI64, 6},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::AllocRefArray, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::RefArrayGet, 0},
        {lang::OpCode::Return, 0},
    };
    lang::VM vm;
    require_throws([&] { (void)test_support::execute_verified(vm, function, describe(function, 6)); },
                   "RefArrayGet index equal to length did not trap deterministically");
}

void verifier_rejects_ref_array_get_on_scalar_array_with_structured_reason() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Object);
    function.local_count = 0;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocArray, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::RefArrayGet, 0},
        {lang::OpCode::Return, 0},
    };

    auto report = lang::verify_with_diagnostics(function);
    require(!report.result.has_value(),
            "verifier accepted RefArrayGet on a ScalarArray receiver");
    require(!report.diagnostics.empty(),
            "verifier rejected malformed RefArray bytecode without diagnostics");
    require(report.diagnostics.front().reason == lang::VerifierReason::BadArrayOperation,
            "malformed RefArray bytecode used the wrong structured verifier reason: " +
                lang::format_verifier_diagnostic(report.diagnostics.front()));
}

void verifier_rejects_scalar_array_get_on_ref_array_with_structured_reason() {
    lang::Function function;
    set_signature(function, {}, lang::ValueKind::Int64);
    function.local_count = 0;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 5},
        {lang::OpCode::ConstantI64, 6},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::AllocRefArray, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ArrayGet, 0},
        {lang::OpCode::Return, 0},
    };

    auto report = lang::verify_with_diagnostics(function);
    require(!report.result.has_value(),
            "verifier accepted ArrayGet on a RefArray receiver");
    require(!report.diagnostics.empty(),
            "verifier rejected malformed scalar array bytecode without diagnostics");
    require(report.diagnostics.front().reason == lang::VerifierReason::BadArrayOperation,
            "malformed scalar array bytecode used the wrong structured verifier reason: " +
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
        {"reachability_through_ref_array_element_survives_and_forwards",
         "RefArray descriptor declares every payload slot as a precise reference",
         "RED before implementation: ObjectKind::RefArray and ref-array scan APIs do not exist",
         reachability_through_ref_array_element_survives_and_forwards},
        {"old_ref_array_to_young_element_store_is_remembered_for_minor_collection",
         "RefArray element stores run the old-to-young write barrier before publishing",
         "RED before implementation: RefArraySet has no barrier hook or remembered-set coverage",
         old_ref_array_to_young_element_store_is_remembered_for_minor_collection},
        {"mixed_heap_pairs_scalar_arrays_and_ref_arrays_compact_together",
         "variable-size compaction advances by descriptor width across all object kinds",
         "RED before implementation: mixed RefArray descriptors cannot be allocated or compacted",
         mixed_heap_pairs_scalar_arrays_and_ref_arrays_compact_together},
        {"descriptor_contrast_ref_array_traces_scalar_array_does_not",
         "descriptor contrast: RefArray payload is traced, ScalarArray raw payload is not",
         "RED before implementation: only zero-reference ScalarArray payload descriptors exist",
         descriptor_contrast_ref_array_traces_scalar_array_does_not},
        {"bytecode_ref_array_get_returns_precise_reference_slot_under_gc_stress",
         "RefArray bytecode stack maps mark allocation and get result slots as references",
         "RED before implementation: RefArray opcodes and verifier kind do not exist",
         bytecode_ref_array_get_returns_precise_reference_slot_under_gc_stress},
        {"ref_array_get_out_of_bounds_traps_deterministically",
         "RefArrayGet performs deterministic runtime bounds checks",
         "RED before implementation: VM has no RefArrayGet trap path",
         ref_array_get_out_of_bounds_traps_deterministically},
        {"verifier_rejects_ref_array_get_on_scalar_array_with_structured_reason",
         "verifier rejects RefArrayGet on ScalarArray with BadArrayOperation",
         "RED before implementation: verifier has no RefArray receiver kind",
         verifier_rejects_ref_array_get_on_scalar_array_with_structured_reason},
        {"verifier_rejects_scalar_array_get_on_ref_array_with_structured_reason",
         "verifier rejects scalar ArrayGet on RefArray with BadArrayOperation",
         "RED before implementation: verifier has no RefArray receiver kind",
         verifier_rejects_scalar_array_get_on_ref_array_with_structured_reason},
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
        std::cerr << failures << " iteration-21 RefArray test(s) failed\n";
        return 1;
    }
    return 0;
}
