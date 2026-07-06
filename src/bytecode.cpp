#include "lang/bytecode.hpp"

#include <cstdint>

namespace lang {

bool verify(const Function& function) {
    std::int64_t depth = 0;
    for (const auto& ins : function.code) {
        switch (ins.op) {
        case OpCode::ConstantI64:
            ++depth;
            break;
        case OpCode::AddI64:
            depth -= 1; // pop two, push one
            break;
        case OpCode::AllocPair:
            depth -= 1; // pop two fields, push object
            break;
        case OpCode::LoadLocal:
            if (ins.operand < 0 || static_cast<std::uint32_t>(ins.operand) >= function.local_count) return false;
            ++depth;
            break;
        case OpCode::StoreLocal:
            if (ins.operand < 0 || static_cast<std::uint32_t>(ins.operand) >= function.local_count) return false;
            --depth;
            break;
        case OpCode::Collect:
            break;
        case OpCode::Return:
            --depth;
            break;
        }
        if (depth < 0) {
            return false;
        }
    }
    return true;
}

} // namespace lang
