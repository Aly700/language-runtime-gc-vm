#include "lang/vm.hpp"

#include <stdexcept>

namespace lang {

Value VM::pop() {
    if (stack_.empty()) {
        throw std::runtime_error("VM stack underflow");
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
    locals_.assign(function.local_count, Value::nil());

    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        const auto& ins = function.code[pc];
        switch (ins.op) {
        case OpCode::ConstantI64:
            push(Value::int64(ins.operand));
            break;
        case OpCode::AddI64: {
            const auto rhs = pop().as_i64();
            const auto lhs = pop().as_i64();
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
            push(locals_.at(static_cast<std::size_t>(ins.operand)));
            break;
        case OpCode::StoreLocal:
            locals_.at(static_cast<std::size_t>(ins.operand)) = pop();
            break;
        case OpCode::Collect: {
            std::vector<Value> roots = stack_;
            roots.insert(roots.end(), locals_.begin(), locals_.end());
            heap_.collect(roots);
            break;
        }
        case OpCode::Return:
            return pop();
        }
    }
    return Value::nil();
}

} // namespace lang
