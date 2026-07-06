#include "lang/bytecode.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lang {

namespace {

enum class AbstractKind {
    Int64,
    Bool,
    Object,
    Nil,
};

bool is_valid_local_index(std::int64_t operand, std::uint32_t local_count) {
    return operand >= 0 && static_cast<std::uint64_t>(operand) < local_count;
}

bool stack_map_matches(const StackMap& map, const std::vector<AbstractKind>& stack) {
    if (map.object_slots.size() != stack.size()) {
        return false;
    }
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const bool is_object = stack[i] == AbstractKind::Object;
        if (map.object_slots[i] != is_object) {
            return false;
        }
    }
    return true;
}

bool pop_any(std::vector<AbstractKind>& stack, AbstractKind* out = nullptr) {
    if (stack.empty()) {
        return false;
    }
    if (out != nullptr) {
        *out = stack.back();
    }
    stack.pop_back();
    return true;
}

bool pop_expect(std::vector<AbstractKind>& stack, AbstractKind expected) {
    AbstractKind actual{};
    if (!pop_any(stack, &actual)) {
        return false;
    }
    return actual == expected;
}

} // namespace

bool verify(const Function& function) {
    if (!function.stack_maps.empty() && function.stack_maps.size() != function.code.size()) {
        return false;
    }

    std::vector<AbstractKind> stack;
    std::vector<std::optional<AbstractKind>> locals(function.local_count);

    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        if (!function.stack_maps.empty() && !stack_map_matches(function.stack_maps[pc], stack)) {
            return false;
        }

        const auto& ins = function.code[pc];
        switch (ins.op) {
        case OpCode::ConstantI64:
            stack.push_back(AbstractKind::Int64);
            break;
        case OpCode::AddI64: {
            if (!pop_expect(stack, AbstractKind::Int64)) {
                return false;
            }
            if (!pop_expect(stack, AbstractKind::Int64)) {
                return false;
            }
            stack.push_back(AbstractKind::Int64);
            break;
        }
        case OpCode::AllocPair: {
            // Pair fields store full tagged Values; the verifier requires initialized operands
            // and records the result as an object reference for precise root maps.
            if (!pop_any(stack)) {
                return false;
            }
            if (!pop_any(stack)) {
                return false;
            }
            stack.push_back(AbstractKind::Object);
            break;
        }
        case OpCode::LoadLocal: {
            if (!is_valid_local_index(ins.operand, function.local_count)) {
                return false;
            }
            const auto local_index = static_cast<std::size_t>(ins.operand);
            if (!locals[local_index].has_value()) {
                return false;
            }
            stack.push_back(*locals[local_index]);
            break;
        }
        case OpCode::StoreLocal: {
            if (!is_valid_local_index(ins.operand, function.local_count)) {
                return false;
            }
            AbstractKind stored{};
            if (!pop_any(stack, &stored)) {
                return false;
            }
            locals[static_cast<std::size_t>(ins.operand)] = stored;
            break;
        }
        case OpCode::Collect:
            break;
        case OpCode::Return:
            if (!pop_any(stack)) {
                return false;
            }
            break;
        }
    }
    return true;
}

} // namespace lang
