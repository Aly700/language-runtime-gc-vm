#include "lang/vm.hpp"

#include <cassert>
#include <cstddef>
#include <stdexcept>

namespace lang {

VM::VM() {
    heap_.set_root_provider(this);
}

void VM::set_gc_stress(gc::StressConfig config) {
    gc_stress_ = config;
    heap_.set_stress_config(config);
}

void VM::trace_roots(gc::RootVisitor& visitor) {
    // VM root layout assumption: every mutator-visible Value lives either on the operand
    // stack or in locals while bytecode runs. The visitor receives mutable slots so a
    // future moving collector can update roots before execution resumes.
    for (auto& value : stack_) {
        visitor.visit(value);
    }
    for (auto& value : locals_) {
        visitor.visit(value);
    }
}

void VM::collect_at_instruction_boundary_if_needed() {
    const auto interval = gc_stress_.collect_every_n_instructions;
    if (interval == 0 || instructions_executed_ == 0 || instructions_executed_ % interval != 0) {
        return;
    }
    heap_.collect();
}

Value VM::pop() {
    if (stack_.empty()) {
        assert(false && "verifier invariant violated: VM stack underflow");
        throw std::runtime_error("VM stack underflow after bytecode verification");
    }
    auto value = stack_.back();
    stack_.pop_back();
    return value;
}

void VM::push(Value value) { stack_.push_back(value); }

Value VM::execute(const Function& function) {
    if (!verify(function)) {
        throw std::runtime_error("bytecode verifier rejected function");
    }
    stack_.clear();
    // Locals start as Nil storage, but LoadLocal is legal only after the verifier has
    // proven a StoreLocal initialized that slot on the linear path to the load.
    locals_.assign(function.local_count, Value::nil());
    instructions_executed_ = 0;

    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        collect_at_instruction_boundary_if_needed();
        const auto& ins = function.code[pc];
        switch (ins.op) {
        case OpCode::ConstantI64:
            push(Value::int64(ins.operand));
            break;
        case OpCode::AddI64: {
            const auto rhs_value = pop();
            const auto lhs_value = pop();
            assert(rhs_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: AddI64 rhs must be i64");
            assert(lhs_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: AddI64 lhs must be i64");
            const auto rhs = rhs_value.as_i64();
            const auto lhs = lhs_value.as_i64();
            push(Value::int64(lhs + rhs));
            break;
        }
        case OpCode::AllocPair: {
            auto right = pop();
            auto left = pop();
            push(Value::object(heap_.allocate_pair(left, right)));
            break;
        }
        case OpCode::LoadLocal:
            assert(ins.operand >= 0 && static_cast<std::size_t>(ins.operand) < locals_.size() &&
                   "verifier invariant violated: LoadLocal index must be in range");
            push(locals_.at(static_cast<std::size_t>(ins.operand)));
            break;
        case OpCode::StoreLocal:
            assert(ins.operand >= 0 && static_cast<std::size_t>(ins.operand) < locals_.size() &&
                   "verifier invariant violated: StoreLocal index must be in range");
            locals_.at(static_cast<std::size_t>(ins.operand)) = pop();
            break;
        case OpCode::Collect: {
            heap_.collect();
            break;
        }
        case OpCode::Return:
            ++instructions_executed_;
            return pop();
        }
        ++instructions_executed_;
    }
    return Value::nil();
}

} // namespace lang
