#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/value.hpp"
#include "lang/vm.hpp"
#include "test_support.hpp"

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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
    case lang::OpCode::AllocRecord:
        return "AllocRecord";
    case lang::OpCode::RecordGet:
        return "RecordGet";
    case lang::OpCode::RecordSet:
        return "RecordSet";
    }
    return "<unknown>";
}

std::string describe(const lang::Function& function) {
    std::ostringstream out;
    out << "locals=" << function.local_count << "\n";
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        const auto& ins = function.code[pc];
        out << "  #" << pc << " " << op_name(ins.op) << " " << ins.operand << "\n";
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

void verifier_rejects_add_i64_stack_underflow_at_instruction() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::AddI64, 0},
    };

    require(!lang::verify(function),
            "verifier accepted AddI64 with only one stack operand\n" + describe(function));
}

void verifier_rejects_uninitialized_local_read() {
    lang::Function function;
    function.local_count = 1;
    function.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(function),
            "verifier accepted LoadLocal from an uninitialized local\n" + describe(function));
}

void verifier_rejects_add_i64_over_object_value() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::ConstantI64, 3},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(function),
            "verifier accepted AddI64 with an object operand\n" + describe(function));
}

void verifier_rejects_invalid_stack_map_for_alloc_pair_result() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Return, 0},
    };
    function.stack_maps = {
        {{}},
        {{false}},
        {{false, false}},
        {{false}},
    };

    require(!lang::verify(function),
            "verifier accepted a stack map that marks AllocPair result as non-object\n" +
                describe(function));
}

void heap_does_not_alias_stale_object_ids_after_sweep() {
    lang::gc::Heap heap;
    VectorRoots no_roots;
    heap.set_root_provider(&no_roots);
    const auto stale = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    heap.collect();
    const auto replacement = heap.allocate_pair(lang::Value::int64(3), lang::Value::int64(4));

    std::ostringstream message;
    message << "swept ObjectId was reused immediately: stale=" << stale
            << " replacement=" << replacement;
    require(stale != replacement, message.str());

    require_throws([&] { (void)heap.object(stale); },
                   "stale ObjectId dereferenced replacement object: stale=" +
                       std::to_string(stale) + " replacement=" + std::to_string(replacement));
}

void heap_traps_when_marking_stale_root() {
    lang::gc::Heap heap;
    VectorRoots no_roots;
    heap.set_root_provider(&no_roots);
    const auto stale = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    heap.collect();

    VectorRoots stale_roots;
    stale_roots.roots.push_back(lang::Value::object(stale));

    std::ostringstream message;
    message << "collector accepted stale root ObjectId " << stale
            << " instead of trapping root-precision bug";
    require_throws([&] { heap.collect(stale_roots); }, message.str());
}

lang::Function single_pair_program() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Return, 0},
    };
    return function;
}

lang::Function nested_pair_program() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::ConstantI64, 3},
        {lang::OpCode::ConstantI64, 4},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Return, 0},
    };
    return function;
}

void collect_returned_value(lang::VM& vm, lang::Value value) {
    VectorRoots roots;
    roots.roots.push_back(value);
    vm.heap().collect(roots);
}

void gc_stress_before_every_allocation_keeps_popped_operands_alive() {
    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    vm.set_gc_stress(stress);

    const auto function = nested_pair_program();
    const auto result = test_support::execute_verified(vm, function, describe(function));
    collect_returned_value(vm, result);

    std::ostringstream message;
    message << "mode=before-every-alloc swept live pair operands\n" << describe(function);
    require(vm.heap().live_count() == 3, message.str());
}

void gc_stress_after_every_allocation_keeps_new_object_alive() {
    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_after_every_allocation = true;
    vm.set_gc_stress(stress);

    const auto function = single_pair_program();
    const auto result = test_support::execute_verified(vm, function, describe(function));
    collect_returned_value(vm, result);

    std::ostringstream message;
    message << "mode=after-every-alloc swept the just-allocated object\n" << describe(function);
    require(vm.heap().live_count() == 1, message.str());
    require(vm.heap().object(result.as_object()).left.as_i64() == 1,
            "mode=after-every-alloc returned object id does not dereference to expected pair");
}

void gc_stress_every_instruction_keeps_stack_roots_alive() {
    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_every_n_instructions = 1;
    vm.set_gc_stress(stress);

    const auto function = single_pair_program();
    const auto result = test_support::execute_verified(vm, function, describe(function));
    collect_returned_value(vm, result);

    std::ostringstream message;
    message << "mode=every-1-instruction swept object while it was on the VM stack\n"
            << describe(function);
    require(vm.heap().live_count() == 1, message.str());
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"verifier_rejects_add_i64_stack_underflow_at_instruction",
         verifier_rejects_add_i64_stack_underflow_at_instruction},
        {"verifier_rejects_uninitialized_local_read", verifier_rejects_uninitialized_local_read},
        {"verifier_rejects_add_i64_over_object_value", verifier_rejects_add_i64_over_object_value},
        {"verifier_rejects_invalid_stack_map_for_alloc_pair_result",
         verifier_rejects_invalid_stack_map_for_alloc_pair_result},
        {"heap_does_not_alias_stale_object_ids_after_sweep",
         heap_does_not_alias_stale_object_ids_after_sweep},
        {"heap_traps_when_marking_stale_root", heap_traps_when_marking_stale_root},
        {"gc_stress_before_every_allocation_keeps_popped_operands_alive",
         gc_stress_before_every_allocation_keeps_popped_operands_alive},
        {"gc_stress_after_every_allocation_keeps_new_object_alive",
         gc_stress_after_every_allocation_keeps_new_object_alive},
        {"gc_stress_every_instruction_keeps_stack_roots_alive",
         gc_stress_every_instruction_keeps_stack_roots_alive},
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
        std::cerr << failures << " phase-1 correctness test(s) failed\n";
        return 1;
    }
    return 0;
}
