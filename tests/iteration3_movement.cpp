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
    case lang::OpCode::Call:
        return "Call";
    case lang::OpCode::Return:
        return "Return";
    }
    return "<unknown>";
}

std::uint32_t slot_of(lang::ObjectId id) {
    return static_cast<std::uint32_t>(id & kSlotMask);
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
        out << " left=" << describe_value(object.left)
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
        << "\n";
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

void require_pair_i64(const lang::gc::Heap& heap, lang::ObjectId id, std::int64_t left,
                      std::int64_t right, const std::string& context) {
    const auto& object = heap.object(id);
    require(object.left.as_i64() == left && object.right.as_i64() == right,
            context + "\n" + describe_heap_object(heap, id, "pair"));
}

void heap_compaction_rewrites_roots_and_cycle_fields() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(100), lang::Value::int64(101));
    const auto first = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    const auto second = heap.allocate_pair(lang::Value::int64(3), lang::Value::int64(4));
    heap.set_left(first, lang::Value::object(second));
    heap.set_right(second, lang::Value::object(first));

    VectorRoots roots;
    roots.roots.push_back(lang::Value::object(first));
    heap.collect(roots);

    const auto moved_first = roots.roots.at(0).as_object();
    require(moved_first != first && slot_of(moved_first) == 0,
            "rooted survivor did not move into the first compacted slot\n" +
                describe_heap(heap, {{"dead", dead}, {"first", first}, {"root", moved_first}}));

    const auto moved_second = heap.object(moved_first).left.as_object();
    require(slot_of(moved_second) == 1,
            "heap field was not rewritten to the second compacted survivor\n" +
                describe_heap(heap, {{"root", moved_first}, {"second", moved_second}}));
    require(heap.object(moved_second).right.as_object() == moved_first,
            "cycle field still points at the pre-compaction first id\n" +
                describe_heap(heap, {{"root", moved_first}, {"second", moved_second}}));
    require(heap.live_count() == 2,
            "compaction should keep exactly the two cycle objects live\n" +
                describe_heap(heap, {{"root", moved_first}, {"second", moved_second}}));
}

void heap_stale_pre_compaction_id_traps_after_move() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(10), lang::Value::int64(11));
    const auto survivor = heap.allocate_pair(lang::Value::int64(20), lang::Value::int64(21));

    auto handle = heap.make_handle(survivor);
    heap.collect();

    const auto moved = handle.object();
    require(moved != survivor && slot_of(moved) == 0,
            "test setup did not move the survivor out of its original slot\n" +
                describe_heap(heap, {{"dead", dead}, {"survivor", survivor}, {"moved", moved}}));
    require_throws([&] { (void)heap.object(survivor); },
                   "pre-compaction ObjectId still dereferenced after survivor moved\n" +
                       describe_heap(heap, {{"survivor", survivor}, {"moved", moved}}));
    require_pair_i64(heap, moved, 20, 21, "moved survivor fields changed during compaction");
}

lang::Function local_root_movement_program() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.local_count = 2;
    function.code = {
        {lang::OpCode::ConstantI64, 100},
        {lang::OpCode::ConstantI64, 101},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::Return, 0},
    };
    return function;
}

void vm_collect_rewrites_local_root_after_movement() {
    const auto function = local_root_movement_program();
    lang::VM vm;
    const auto result = vm.execute(function);

    require(result.is_object(), "local-root movement program did not return an object\n" +
                                    describe(function, 10));
    require(slot_of(result.as_object()) == 0,
            "Collect did not rewrite the moved object id stored in a VM local\n" +
                describe(function, 10) +
                describe_heap(vm.heap(), {{"result", result.as_object()}}));
    require_pair_i64(vm.heap(), result.as_object(), 1, 2,
                     "local-root object fields changed during compaction");
}

lang::Function stack_root_movement_program() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.local_count = 1;
    function.code = {
        {lang::OpCode::ConstantI64, 100},
        {lang::OpCode::ConstantI64, 101},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 5},
        {lang::OpCode::ConstantI64, 6},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::Return, 0},
    };
    return function;
}

void vm_collect_rewrites_stack_root_after_movement() {
    const auto function = stack_root_movement_program();
    lang::VM vm;
    const auto result = vm.execute(function);

    require(result.is_object(), "stack-root movement program did not return an object\n" +
                                    describe(function, 9));
    require(slot_of(result.as_object()) == 0,
            "Collect did not rewrite the moved object id on the VM operand stack\n" +
                describe(function, 9) +
                describe_heap(vm.heap(), {{"result", result.as_object()}}));
    require_pair_i64(vm.heap(), result.as_object(), 5, 6,
                     "stack-root object fields changed during compaction");
}

lang::Function interleaved_loop_program() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.local_count = 3;
    function.code = {
        {lang::OpCode::ConstantI64, -1}, // 0: initial head.left
        {lang::OpCode::ConstantI64, 0},  // 1
        {lang::OpCode::AllocPair, 0},    // 2
        {lang::OpCode::StoreLocal, 0},   // 3: head
        {lang::OpCode::ConstantI64, 0},  // 4
        {lang::OpCode::StoreLocal, 1},   // 5: i
        {lang::OpCode::LoadLocal, 1},    // 6: loop header
        {lang::OpCode::ConstantI64, 3},  // 7
        {lang::OpCode::LessI64, 0},      // 8
        {lang::OpCode::JumpIfFalse, 25}, // 9
        {lang::OpCode::ConstantI64, 700},// 10: filler.left
        {lang::OpCode::LoadLocal, 1},    // 11: filler.right
        {lang::OpCode::AllocPair, 0},    // 12
        {lang::OpCode::StoreLocal, 2},   // 13: filler creates a low hole when dropped
        {lang::OpCode::LoadLocal, 1},    // 14: new head.left = i
        {lang::OpCode::LoadLocal, 0},    // 15: new head.right = old head
        {lang::OpCode::AllocPair, 0},    // 16
        {lang::OpCode::StoreLocal, 0},   // 17
        {lang::OpCode::ConstantI64, 0},  // 18
        {lang::OpCode::StoreLocal, 2},   // 19: drop filler; next stress collect compacts
        {lang::OpCode::LoadLocal, 1},    // 20
        {lang::OpCode::ConstantI64, 1},  // 21
        {lang::OpCode::AddI64, 0},       // 22
        {lang::OpCode::StoreLocal, 1},   // 23
        {lang::OpCode::Jump, 6},         // 24
        {lang::OpCode::LoadLocal, 0},    // 25
        {lang::OpCode::Return, 0},       // 26
    };
    return function;
}

void gc_every_instruction_compacts_interleaved_loop_and_preserves_live_chain() {
    const auto function = interleaved_loop_program();
    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_every_n_instructions = 1;
    vm.set_gc_stress(stress);

    const auto result = vm.execute(function);
    require(result.is_object(), "interleaved loop did not return an object\n" +
                                    describe(function, 26));
    require(slot_of(result.as_object()) == 3,
            "every-instruction stress did not compact across dropped loop fillers\n" +
                describe(function, 19) +
                describe_heap(vm.heap(), {{"result", result.as_object()}}));
    require(vm.heap().live_count() == 4,
            "every-instruction stress should leave exactly the four list nodes live\n" +
                describe(function, 16) +
                describe_heap(vm.heap(), {{"result", result.as_object()}}));

    auto node = result.as_object();
    for (std::int64_t expected = 2; expected >= 0; --expected) {
        const auto& object = vm.heap().object(node);
        require(object.left.as_i64() == expected,
                "compacted list node has wrong left value\n" + describe(function, 16) +
                    describe_heap_object(vm.heap(), node, "node"));
        node = object.right.as_object();
    }
    require(vm.heap().object(node).left.as_i64() == -1,
            "compacted list tail was not preserved\n" + describe(function, 16) +
                describe_heap_object(vm.heap(), node, "tail"));
}

void deep_chain_compaction_uses_iterative_marking_worklist() {
    constexpr std::int64_t kDepth = 1024;

    lang::gc::Heap heap;
    const auto filler = heap.allocate_pair(lang::Value::int64(900), lang::Value::int64(901));
    auto head = heap.allocate_pair(lang::Value::int64(0), lang::Value::nil());
    for (std::int64_t i = 1; i < kDepth; ++i) {
        head = heap.allocate_pair(lang::Value::int64(i), lang::Value::object(head));
    }

    VectorRoots roots;
    roots.roots.push_back(lang::Value::object(head));
    heap.collect(roots);

    const auto moved_head = roots.roots.at(0).as_object();
    require(slot_of(moved_head) == static_cast<std::uint32_t>(kDepth - 1),
            "deep chain did not compact across the dropped filler slot\n" +
                describe_heap(heap, {{"filler", filler}, {"old_head", head},
                                     {"moved_head", moved_head}}));
    require(heap.live_count() == static_cast<std::size_t>(kDepth),
            "deep chain collection kept the wrong number of live nodes\n" +
                describe_heap(heap, {{"moved_head", moved_head}}));

    auto node = moved_head;
    for (std::int64_t expected = kDepth - 1; expected >= 0; --expected) {
        const auto& object = heap.object(node);
        require(object.left.as_i64() == expected,
                "deep chain node value changed during compaction\n" +
                    describe_heap_object(heap, node, "node"));
        if (expected == 0) {
            require(object.right.tag() == lang::Value::Tag::Nil,
                    "deep chain tail should end in nil\n" +
                        describe_heap_object(heap, node, "tail"));
        } else {
            node = object.right.as_object();
        }
    }
}

void heap_compaction_updates_moved_self_cycle() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    const auto self = heap.allocate_pair(lang::Value::int64(3), lang::Value::int64(4));
    heap.set_left(self, lang::Value::object(self));

    VectorRoots roots;
    roots.roots.push_back(lang::Value::object(self));
    heap.collect(roots);

    const auto moved = roots.roots.at(0).as_object();
    require(moved != self && slot_of(moved) == 0,
            "self-cycle survivor did not move into the first compacted slot\n" +
                describe_heap(heap, {{"dead", dead}, {"self", self}, {"moved", moved}}));
    require(heap.object(moved).left.as_object() == moved,
            "self-cycle field was not rewritten to the moved ObjectId\n" +
                describe_heap(heap, {{"self", self}, {"moved", moved}}));
    require_throws([&] { (void)heap.object(self); },
                   "pre-compaction self ObjectId still dereferenced after movement\n" +
                       describe_heap(heap, {{"self", self}, {"moved", moved}}));
}

lang::Function before_alloc_operand_movement_program() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.local_count = 3;
    function.code = {
        {lang::OpCode::ConstantI64, 900},
        {lang::OpCode::ConstantI64, 901},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},   // filler kept live until final allocation
        {lang::OpCode::ConstantI64, 11},
        {lang::OpCode::ConstantI64, 12},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 1},   // left operand
        {lang::OpCode::ConstantI64, 21},
        {lang::OpCode::ConstantI64, 22},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 2},   // right operand
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 1},   // left now only lives on the stack
        {lang::OpCode::LoadLocal, 2},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 2},   // right now only lives on the stack
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 0},   // free slot 0 before the final AllocPair
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Return, 0},
    };
    return function;
}

void before_alloc_stress_rewrites_popped_alloc_pair_operands() {
    const auto function = before_alloc_operand_movement_program();
    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    vm.set_gc_stress(stress);

    const auto result = vm.execute(function);
    require(result.is_object(), "before-allocation operand movement program returned non-object\n" +
                                    describe(function, 20));

    const auto& pair = vm.heap().object(result.as_object());
    const auto left = pair.left.as_object();
    const auto right = pair.right.as_object();
    require(slot_of(left) == 0 && slot_of(right) == 1 && slot_of(result.as_object()) == 2,
            "before-allocation stress did not rewrite popped operands before publishing fields\n" +
                describe(function, 20) +
                describe_heap(vm.heap(), {{"result", result.as_object()},
                                          {"left", left},
                                          {"right", right}}));
    require_pair_i64(vm.heap(), left, 11, 12,
                     "left popped operand did not survive movement with correct fields");
    require_pair_i64(vm.heap(), right, 21, 22,
                     "right popped operand did not survive movement with correct fields");
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"heap_compaction_rewrites_roots_and_cycle_fields",
         heap_compaction_rewrites_roots_and_cycle_fields},
        {"heap_stale_pre_compaction_id_traps_after_move",
         heap_stale_pre_compaction_id_traps_after_move},
        {"vm_collect_rewrites_local_root_after_movement",
         vm_collect_rewrites_local_root_after_movement},
        {"vm_collect_rewrites_stack_root_after_movement",
         vm_collect_rewrites_stack_root_after_movement},
        {"gc_every_instruction_compacts_interleaved_loop_and_preserves_live_chain",
         gc_every_instruction_compacts_interleaved_loop_and_preserves_live_chain},
        {"deep_chain_compaction_uses_iterative_marking_worklist",
         deep_chain_compaction_uses_iterative_marking_worklist},
        {"heap_compaction_updates_moved_self_cycle", heap_compaction_updates_moved_self_cycle},
        {"before_alloc_stress_rewrites_popped_alloc_pair_operands",
         before_alloc_stress_rewrites_popped_alloc_pair_operands},
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
        std::cerr << failures << " iteration-3 movement test(s) failed\n";
        return 1;
    }
    return 0;
}
