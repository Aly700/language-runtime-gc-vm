#include "lang/bytecode.hpp"
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

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

lang::Function linked_structure_loop_program() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.local_count = 2;
    function.code = {
        {lang::OpCode::ConstantI64, 0},  // 0: initial head.left
        {lang::OpCode::ConstantI64, 0},  // 1: initial head.right
        {lang::OpCode::AllocPair, 0},    // 2
        {lang::OpCode::StoreLocal, 0},   // 3: head
        {lang::OpCode::ConstantI64, 0},  // 4
        {lang::OpCode::StoreLocal, 1},   // 5: i
        {lang::OpCode::LoadLocal, 1},    // 6: loop header
        {lang::OpCode::ConstantI64, 4},  // 7
        {lang::OpCode::LessI64, 0},      // 8
        {lang::OpCode::JumpIfFalse, 19}, // 9
        {lang::OpCode::ConstantI64, 0},  // 10: new.left
        {lang::OpCode::LoadLocal, 0},    // 11: new.right = old head
        {lang::OpCode::AllocPair, 0},    // 12
        {lang::OpCode::StoreLocal, 0},   // 13: head = new
        {lang::OpCode::LoadLocal, 1},    // 14
        {lang::OpCode::ConstantI64, 1},  // 15
        {lang::OpCode::AddI64, 0},       // 16
        {lang::OpCode::StoreLocal, 1},   // 17
        {lang::OpCode::Jump, 6},         // 18
        {lang::OpCode::LoadLocal, 0},    // 19
        {lang::OpCode::Return, 0},       // 20
    };
    return function;
}

void verifier_rejects_branch_to_out_of_bounds_target() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 99},
        {lang::OpCode::ConstantI64, 7},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(function),
            "verifier accepted out-of-bounds branch target\n" + describe(function, 3));
}

void verifier_rejects_stack_height_mismatch_at_merge() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 6},
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::Jump, 6},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(function),
            "verifier accepted merge with different stack heights\n" + describe(function, 6));
}

void verifier_rejects_local_initialized_on_one_path_only_then_loaded() {
    lang::Function function;
    function.local_count = 1;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 7},
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::Jump, 7},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(function),
            "verifier accepted LoadLocal after path-partial initialization\n" +
                describe(function, 7));
}

void verifier_rejects_type_mismatch_at_join_then_misused() {
    lang::Function function;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 7},
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::Jump, 10},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(function),
            "verifier accepted merged i64/object stack value used as i64\n" +
                describe(function, 11));
}

void verifier_accepts_loop_that_builds_linked_structure() {
    const auto function = linked_structure_loop_program();

    const auto verification = lang::verify_with_stack_maps(function);
    require(verification.has_value(),
            "verifier rejected valid linked-structure loop\n" + describe(function, 6));
    require(verification->stack_maps.size() == function.code.size(),
            "verifier did not generate one stack map per pc\n" + describe(function, 6));

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_every_n_instructions = 1;
    vm.set_gc_stress(stress);
    const auto result = test_support::execute_verified(vm, function,
                                                       describe(function, 0));
    require(result.is_object(), "linked-structure loop did not return an object");
    require(vm.heap().live_count() == 5,
            "linked-structure loop should keep all five linked pairs reachable\n" +
                describe(function, 20));
}

void field_load_reads_object_field_with_generated_stack_map() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::GetLeft, 0},
        {lang::OpCode::Return, 0},
    };

    const auto verification = lang::verify_with_stack_maps(function);
    require(verification.has_value(),
            "verifier rejected object field load\n" + describe(function, 5));
    require(verification->stack_maps.at(6).object_slots == std::vector<bool>{true},
            "GetLeft object result did not generate an object stack map\n" +
                describe(function, 6));

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_every_n_instructions = 1;
    vm.set_gc_stress(stress);
    const auto result = test_support::execute_verified(vm, function,
                                                       describe(function, 0));
    require(result.is_object(), "GetLeft did not return the inner object");
    require(vm.heap().live_count() == 1,
            "every-instruction stress should keep only the loaded object reachable\n" +
                describe(function, 6));
}

void gc_collects_unreachable_cycle_created_by_mutation() {
    lang::Function function;
    function.local_count = 2;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::SetLeft, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::SetRight, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::ConstantI64, 7},
        {lang::OpCode::Return, 0},
    };

    lang::VM vm;
    const auto result = test_support::execute_verified(vm, function,
                                                       describe(function, 0));
    require(result.as_i64() == 7, "cycle collection program returned wrong value");
    require(vm.heap().live_count() == 0,
            "unreachable cycle created by SetLeft/SetRight leaked after Collect\n" +
                describe(function, 18));
}

void gc_instruction_stress_sweeps_replaced_object_after_it_becomes_unreachable() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.local_count = 2;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::ConstantI64, 4},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 22},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 99},
        {lang::OpCode::SetRight, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::Jump, 6},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
    };

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_every_n_instructions = 1;
    vm.set_gc_stress(stress);
    const auto result = test_support::execute_verified(vm, function,
                                                       describe(function, 0));
    require(result.is_object(), "replacement loop did not return final object");
    require(vm.heap().live_count() == 1,
            "every-instruction stress did not sweep replaced objects promptly\n" +
                describe(function, 16));
}

void gc_self_referential_pair_survives_while_rooted() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Object;
    function.local_count = 1;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::SetLeft, 0},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
    };

    lang::VM vm;
    const auto result = test_support::execute_verified(vm, function,
                                                       describe(function, 0));
    require(result.is_object(), "self-reference program did not return an object");
    require(vm.heap().live_count() == 1,
            "rooted self-referential pair was swept\n" + describe(function, 7));
    require(vm.heap().object(result.as_object()).left.as_object() == result.as_object(),
            "self-referential pair did not retain its left self-reference");
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"verifier_rejects_branch_to_out_of_bounds_target",
         verifier_rejects_branch_to_out_of_bounds_target},
        {"verifier_rejects_stack_height_mismatch_at_merge",
         verifier_rejects_stack_height_mismatch_at_merge},
        {"verifier_rejects_local_initialized_on_one_path_only_then_loaded",
         verifier_rejects_local_initialized_on_one_path_only_then_loaded},
        {"verifier_rejects_type_mismatch_at_join_then_misused",
         verifier_rejects_type_mismatch_at_join_then_misused},
        {"verifier_accepts_loop_that_builds_linked_structure",
         verifier_accepts_loop_that_builds_linked_structure},
        {"field_load_reads_object_field_with_generated_stack_map",
         field_load_reads_object_field_with_generated_stack_map},
        {"gc_collects_unreachable_cycle_created_by_mutation",
         gc_collects_unreachable_cycle_created_by_mutation},
        {"gc_instruction_stress_sweeps_replaced_object_after_it_becomes_unreachable",
         gc_instruction_stress_sweeps_replaced_object_after_it_becomes_unreachable},
        {"gc_self_referential_pair_survives_while_rooted",
         gc_self_referential_pair_survives_while_rooted},
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
        std::cerr << failures << " iteration-2 correctness test(s) failed\n";
        return 1;
    }
    return 0;
}
