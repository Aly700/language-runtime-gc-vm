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

void VM::assert_stack_matches_map(const VerificationResult& verification, std::size_t pc) const {
    assert(pc < verification.stack_maps.size() &&
           "verifier invariant violated: missing stack map for pc");
    const auto& map = verification.stack_maps[pc];
    assert(map.object_slots.size() == stack_.size() &&
           "verifier invariant violated: runtime stack height differs from stack map");
    for (std::size_t i = 0; i < stack_.size(); ++i) {
        assert(map.object_slots[i] == stack_[i].is_object() &&
               "verifier invariant violated: runtime stack tag differs from stack map");
    }
}

void VM::collect_at_instruction_boundary_if_needed(const VerificationResult& verification,
                                                   std::size_t pc) {
    const auto interval = gc_stress_.collect_every_n_instructions;
    if (interval == 0 || instructions_executed_ == 0 || instructions_executed_ % interval != 0) {
        return;
    }
    assert_stack_matches_map(verification, pc);
    heap_.collect();
    assert_stack_matches_map(verification, pc);
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
    auto verification = verify_with_stack_maps(function);
    if (!verification.has_value()) {
        throw std::runtime_error("bytecode verifier rejected function");
    }
    stack_.clear();
    // Locals start as Nil storage, but LoadLocal is legal only after the verifier has
    // proven a StoreLocal initialized that slot on the linear path to the load.
    locals_.assign(function.local_count, Value::nil());
    instructions_executed_ = 0;

    std::size_t pc = 0;
    while (pc < function.code.size()) {
        assert_stack_matches_map(*verification, pc);
        collect_at_instruction_boundary_if_needed(*verification, pc);
        const auto& ins = function.code[pc];
        switch (ins.op) {
        case OpCode::ConstantI64:
            push(Value::int64(ins.operand));
            ++pc;
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
            ++pc;
            break;
        }
        case OpCode::LessI64: {
            const auto rhs_value = pop();
            const auto lhs_value = pop();
            assert(rhs_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: LessI64 rhs must be i64");
            assert(lhs_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: LessI64 lhs must be i64");
            push(Value::boolean(lhs_value.as_i64() < rhs_value.as_i64()));
            ++pc;
            break;
        }
        case OpCode::AllocPair: {
            auto right = pop();
            auto left = pop();
            push(Value::object(heap_.allocate_pair(left, right)));
            ++pc;
            break;
        }
        case OpCode::GetLeft: {
            const auto receiver = pop();
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: GetLeft receiver must be object");
            push(heap_.left(receiver.as_object()));
            ++pc;
            break;
        }
        case OpCode::GetRight: {
            const auto receiver = pop();
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: GetRight receiver must be object");
            push(heap_.right(receiver.as_object()));
            ++pc;
            break;
        }
        case OpCode::SetLeft: {
            const auto value = pop();
            const auto receiver = pop();
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: SetLeft receiver must be object");
            heap_.set_left(receiver.as_object(), value);
            ++pc;
            break;
        }
        case OpCode::SetRight: {
            const auto value = pop();
            const auto receiver = pop();
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: SetRight receiver must be object");
            heap_.set_right(receiver.as_object(), value);
            ++pc;
            break;
        }
        case OpCode::LoadLocal:
            assert(ins.operand >= 0 && static_cast<std::size_t>(ins.operand) < locals_.size() &&
                   "verifier invariant violated: LoadLocal index must be in range");
            push(locals_.at(static_cast<std::size_t>(ins.operand)));
            ++pc;
            break;
        case OpCode::StoreLocal:
            assert(ins.operand >= 0 && static_cast<std::size_t>(ins.operand) < locals_.size() &&
                   "verifier invariant violated: StoreLocal index must be in range");
            locals_.at(static_cast<std::size_t>(ins.operand)) = pop();
            ++pc;
            break;
        case OpCode::Jump:
            assert(ins.operand >= 0 && static_cast<std::size_t>(ins.operand) < function.code.size() &&
                   "verifier invariant violated: Jump target must be in range");
            pc = static_cast<std::size_t>(ins.operand);
            break;
        case OpCode::JumpIfFalse: {
            const auto condition = pop();
            assert(condition.tag() == Value::Tag::Bool &&
                   "verifier invariant violated: JumpIfFalse condition must be bool");
            if (!condition.as_bool()) {
                assert(ins.operand >= 0 &&
                       static_cast<std::size_t>(ins.operand) < function.code.size() &&
                       "verifier invariant violated: JumpIfFalse target must be in range");
                pc = static_cast<std::size_t>(ins.operand);
            } else {
                ++pc;
            }
            break;
        }
        case OpCode::Collect: {
            assert_stack_matches_map(*verification, pc);
            heap_.collect();
            assert_stack_matches_map(*verification, pc);
            ++pc;
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
