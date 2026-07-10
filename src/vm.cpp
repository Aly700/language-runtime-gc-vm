#include "lang/vm.hpp"

#include <cassert>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace lang {

VM::VM() {
    heap_.set_root_provider(this);
}

void VM::set_gc_stress(gc::StressConfig config) {
    gc_stress_ = config;
    heap_.set_stress_config(config);
}

void VM::trace_roots(gc::RootVisitor& visitor) {
    // Frame root layout assumption: every mutator-visible Value lives in exactly one
    // live frame's operand stack or locals while bytecode runs. The visitor receives
    // mutable slots so moving collection rewrites active and suspended frames alike.
    for (auto& frame : frames_) {
        for (auto& value : frame.stack) {
            visitor.visit(value);
        }
        for (auto& value : frame.locals) {
            visitor.visit(value);
        }
    }
}

void VM::assert_stack_matches_map(const ModuleVerificationResult& verification,
                                  const Frame& frame) const {
    assert(frame.function_index < verification.functions.size() &&
           "verifier invariant violated: missing function verification result");
    const auto& function_verification = verification.functions[frame.function_index];
    assert(frame.pc < function_verification.stack_maps.size() &&
           "verifier invariant violated: missing stack map for pc");
    const auto& map = function_verification.stack_maps[frame.pc];
    assert(map.object_slots.size() == frame.stack.size() &&
           "verifier invariant violated: runtime stack height differs from stack map");
    for (std::size_t i = 0; i < frame.stack.size(); ++i) {
        if (map.object_slots[i]) {
            assert((frame.stack[i].is_object() ||
                    frame.stack[i].tag() == Value::Tag::Nil) &&
                   "verifier invariant violated: runtime stack reference slot differs from stack map");
        } else {
            assert(!frame.stack[i].is_object() &&
                   "verifier invariant violated: runtime stack object tag differs from stack map");
        }
    }
}

void VM::collect_at_instruction_boundary_if_needed(const ModuleVerificationResult& verification,
                                                   const Frame& frame) {
    const auto major_interval = gc_stress_.collect_every_n_instructions;
    if (major_interval != 0 && instructions_executed_ != 0 &&
        instructions_executed_ % major_interval == 0) {
        assert_stack_matches_map(verification, frame);
        heap_.collect();
        assert_stack_matches_map(verification, frame);
    }

    const auto minor_interval = gc_stress_.collect_minor_every_n_instructions;
    if (minor_interval != 0 && instructions_executed_ != 0 &&
        instructions_executed_ % minor_interval == 0) {
        assert_stack_matches_map(verification, frame);
        heap_.collect_minor();
        assert_stack_matches_map(verification, frame);
    }
}

Value VM::pop(Frame& frame) {
    if (frame.stack.empty()) {
        assert(false && "verifier invariant violated: VM stack underflow");
        throw std::runtime_error("VM stack underflow after bytecode verification");
    }
    auto value = frame.stack.back();
    frame.stack.pop_back();
    return value;
}

void VM::push(Frame& frame, Value value) {
    frame.stack.push_back(value);
}

Value VM::execute(const Function& function) {
    ++raw_function_executions_;
    Module module;
    module.entry_function = 0;
    module.functions.push_back(function);
    return execute_unverified_module(module);
}

void VM::push_frame(const Module& module, std::size_t function_index,
                    std::vector<Value> arguments) {
    if (frames_.size() >= max_call_depth_) {
        throw std::runtime_error("VM call depth limit exceeded");
    }
    if (function_index >= module.functions.size()) {
        throw std::runtime_error("VM attempted to call out-of-range function");
    }

    const auto& function = module.functions[function_index];
    if (arguments.size() != function.signature.parameters.size()) {
        throw std::runtime_error("VM call argument count mismatch after bytecode verification");
    }
    if (function.local_count < arguments.size()) {
        throw std::runtime_error("VM callee local count is smaller than parameter count");
    }

    Frame frame;
    frame.function_index = function_index;
    frame.pc = 0;
    frame.locals.assign(function.local_count, Value::nil());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        frame.locals[i] = arguments[i];
    }
    frames_.push_back(std::move(frame));
}

Value VM::execute(const Module& module) {
    ++raw_module_executions_;
    return execute_unverified_module(module);
}

Value VM::execute_unverified_module(const Module& module) {
    auto verification_report = verify_module_with_diagnostics(module);
    if (!verification_report.module.has_value()) {
        std::string message = "bytecode verifier rejected module";
        if (!verification_report.diagnostics.empty()) {
            message += ": " +
                       format_verifier_diagnostic(verification_report.diagnostics.front());
        }
        throw std::runtime_error(message);
    }
    return execute(*verification_report.module);
}

Value VM::execute(const VerifiedModule& module) {
    return execute_verified(module.module(), module.verification());
}

Value VM::execute_verified(const Module& module,
                           const ModuleVerificationResult& verification) {
    frames_.clear();
    instructions_executed_ = 0;
    push_frame(module, module.entry_function, {});

    while (!frames_.empty()) {
        auto& frame = frames_.back();
        const auto& function = module.functions[frame.function_index];
        assert_stack_matches_map(verification, frame);
        collect_at_instruction_boundary_if_needed(verification, frame);
        const auto& ins = function.code[frame.pc];
        switch (ins.op) {
        case OpCode::ConstantI64:
            push(frame, Value::int64(ins.operand));
            ++frame.pc;
            break;
        case OpCode::PushStr: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       module.string_constants.size() &&
                   "verifier invariant violated: PushStr pool index must be in range");
            const auto& constant =
                module.string_constants[static_cast<std::size_t>(ins.operand)];
            const auto bytes = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(constant.data()),
                constant.size());
            push(frame, Value::object(heap_.allocate_string(bytes)));
            ++frame.pc;
            break;
        }
        case OpCode::StrLen: {
            const auto receiver = pop(frame);
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: StrLen receiver must be object");
            const auto length = heap_.string_length(receiver.as_object());
            assert(length <= static_cast<std::size_t>(
                                 std::numeric_limits<std::int64_t>::max()) &&
                   "string header length must fit in i64");
            push(frame, Value::int64(static_cast<std::int64_t>(length)));
            ++frame.pc;
            break;
        }
        case OpCode::StrEq: {
            const auto right = pop(frame);
            const auto left = pop(frame);
            assert(right.tag() == Value::Tag::Object &&
                   "verifier invariant violated: StrEq rhs must be object");
            assert(left.tag() == Value::Tag::Object &&
                   "verifier invariant violated: StrEq lhs must be object");
            push(frame, Value::boolean(
                            heap_.string_equal(left.as_object(), right.as_object())));
            ++frame.pc;
            break;
        }
        case OpCode::StrConcat: {
            assert(frame.stack.size() >= 2 &&
                   "verifier invariant violated: StrConcat requires two operands");
            const auto left = frame.stack[frame.stack.size() - 2];
            const auto right = frame.stack[frame.stack.size() - 1];
            assert(right.tag() == Value::Tag::Object &&
                   "verifier invariant violated: StrConcat rhs must be object");
            assert(left.tag() == Value::Tag::Object &&
                   "verifier invariant violated: StrConcat lhs must be object");
            const auto result = heap_.allocate_string_concat(left, right);
            (void)pop(frame);
            (void)pop(frame);
            push(frame, Value::object(result));
            ++frame.pc;
            break;
        }
        case OpCode::StrIndex: {
            const auto index_value = pop(frame);
            const auto receiver = pop(frame);
            assert(index_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: StrIndex index must be i64");
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: StrIndex receiver must be object");
            const auto index = index_value.as_i64();
            if (index < 0) {
                throw std::out_of_range("string index out of bounds");
            }
            push(frame, Value::int64(static_cast<std::int64_t>(
                            heap_.string_index(receiver.as_object(),
                                               static_cast<std::size_t>(index)))));
            ++frame.pc;
            break;
        }
        case OpCode::Nil:
            push(frame, Value::nil());
            ++frame.pc;
            break;
        case OpCode::IsNil: {
            const auto value = pop(frame);
            assert((value.tag() == Value::Tag::Object ||
                    value.tag() == Value::Tag::Nil) &&
                   "verifier invariant violated: IsNil operand must be a reference slot");
            push(frame, Value::boolean(value.tag() == Value::Tag::Nil));
            ++frame.pc;
            break;
        }
        case OpCode::AddI64: {
            const auto rhs_value = pop(frame);
            const auto lhs_value = pop(frame);
            assert(rhs_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: AddI64 rhs must be i64");
            assert(lhs_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: AddI64 lhs must be i64");
            const auto rhs = rhs_value.as_i64();
            const auto lhs = lhs_value.as_i64();
            push(frame, Value::int64(lhs + rhs));
            ++frame.pc;
            break;
        }
        case OpCode::LessI64: {
            const auto rhs_value = pop(frame);
            const auto lhs_value = pop(frame);
            assert(rhs_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: LessI64 rhs must be i64");
            assert(lhs_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: LessI64 lhs must be i64");
            push(frame, Value::boolean(lhs_value.as_i64() < rhs_value.as_i64()));
            ++frame.pc;
            break;
        }
        case OpCode::AllocPair: {
            auto right = pop(frame);
            auto left = pop(frame);
            push(frame, Value::object(heap_.allocate_pair(left, right)));
            ++frame.pc;
            break;
        }
        case OpCode::AllocArray: {
            const auto init_value = pop(frame);
            const auto length_value = pop(frame);
            assert(init_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: AllocArray init must be i64");
            assert(length_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: AllocArray length must be i64");
            const auto length = length_value.as_i64();
            if (length < 0) {
                throw std::out_of_range("scalar array length out of bounds");
            }
            push(frame, Value::object(heap_.allocate_scalar_array(
                            static_cast<std::size_t>(length), init_value.as_i64())));
            ++frame.pc;
            break;
        }
        case OpCode::ArrayGet: {
            const auto index_value = pop(frame);
            const auto receiver = pop(frame);
            assert(index_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: ArrayGet index must be i64");
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: ArrayGet receiver must be object");
            const auto index = index_value.as_i64();
            if (index < 0) {
                throw std::out_of_range("scalar array index out of bounds");
            }
            push(frame, Value::int64(heap_.array_get(receiver.as_object(),
                                                     static_cast<std::size_t>(index))));
            ++frame.pc;
            break;
        }
        case OpCode::ArraySet: {
            const auto stored_value = pop(frame);
            const auto index_value = pop(frame);
            const auto receiver = pop(frame);
            assert(stored_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: ArraySet value must be i64");
            assert(index_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: ArraySet index must be i64");
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: ArraySet receiver must be object");
            const auto index = index_value.as_i64();
            if (index < 0) {
                throw std::out_of_range("scalar array index out of bounds");
            }
            heap_.array_set(receiver.as_object(), static_cast<std::size_t>(index),
                            stored_value.as_i64());
            ++frame.pc;
            break;
        }
        case OpCode::ArrayLen: {
            const auto receiver = pop(frame);
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: ArrayLen receiver must be object");
            const auto object = receiver.as_object();
            const auto length =
                heap_.object(object).kind == gc::ObjectKind::RefArray
                    ? heap_.ref_array_length(object)
                    : heap_.array_length(object);
            if (length > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::length_error("scalar array length exceeds i64 result range");
            }
            push(frame, Value::int64(static_cast<std::int64_t>(length)));
            ++frame.pc;
            break;
        }
        case OpCode::AllocRefArray: {
            const auto init_value = pop(frame);
            const auto length_value = pop(frame);
            assert(init_value.tag() == Value::Tag::Object &&
                   "verifier invariant violated: AllocRefArray init must be an object reference");
            assert(length_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: AllocRefArray length must be i64");
            const auto length = length_value.as_i64();
            if (length < 0) {
                throw std::out_of_range("ref array length out of bounds");
            }
            push(frame, Value::object(heap_.allocate_ref_array(
                            static_cast<std::size_t>(length), init_value)));
            ++frame.pc;
            break;
        }
        case OpCode::RefArrayGet: {
            const auto index_value = pop(frame);
            const auto receiver = pop(frame);
            assert(index_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: RefArrayGet index must be i64");
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: RefArrayGet receiver must be object");
            const auto index = index_value.as_i64();
            if (index < 0) {
                throw std::out_of_range("ref array index out of bounds");
            }
            push(frame, heap_.ref_array_get(receiver.as_object(),
                                            static_cast<std::size_t>(index)));
            ++frame.pc;
            break;
        }
        case OpCode::RefArraySet: {
            const auto stored_value = pop(frame);
            const auto index_value = pop(frame);
            const auto receiver = pop(frame);
            assert(stored_value.tag() == Value::Tag::Object &&
                   "verifier invariant violated: RefArraySet value must be an object reference");
            assert(index_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: RefArraySet index must be i64");
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: RefArraySet receiver must be object");
            const auto index = index_value.as_i64();
            if (index < 0) {
                throw std::out_of_range("ref array index out of bounds");
            }
            heap_.ref_array_set(receiver.as_object(), static_cast<std::size_t>(index),
                                stored_value);
            ++frame.pc;
            break;
        }
        case OpCode::GetLeft: {
            const auto receiver = pop(frame);
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: GetLeft receiver must be object");
            push(frame, heap_.left(receiver.as_object()));
            ++frame.pc;
            break;
        }
        case OpCode::GetRight: {
            const auto receiver = pop(frame);
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: GetRight receiver must be object");
            push(frame, heap_.right(receiver.as_object()));
            ++frame.pc;
            break;
        }
        case OpCode::SetLeft: {
            const auto value = pop(frame);
            const auto receiver = pop(frame);
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: SetLeft receiver must be object");
            heap_.set_left(receiver.as_object(), value);
            ++frame.pc;
            break;
        }
        case OpCode::SetRight: {
            const auto value = pop(frame);
            const auto receiver = pop(frame);
            assert(receiver.tag() == Value::Tag::Object &&
                   "verifier invariant violated: SetRight receiver must be object");
            heap_.set_right(receiver.as_object(), value);
            ++frame.pc;
            break;
        }
        case OpCode::LoadLocal:
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) < frame.locals.size() &&
                   "verifier invariant violated: LoadLocal index must be in range");
            push(frame, frame.locals.at(static_cast<std::size_t>(ins.operand)));
            ++frame.pc;
            break;
        case OpCode::StoreLocal:
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) < frame.locals.size() &&
                   "verifier invariant violated: StoreLocal index must be in range");
            frame.locals.at(static_cast<std::size_t>(ins.operand)) = pop(frame);
            ++frame.pc;
            break;
        case OpCode::Jump:
            assert(ins.operand >= 0 && static_cast<std::size_t>(ins.operand) < function.code.size() &&
                   "verifier invariant violated: Jump target must be in range");
            frame.pc = static_cast<std::size_t>(ins.operand);
            break;
        case OpCode::JumpIfFalse: {
            const auto condition = pop(frame);
            assert(condition.tag() == Value::Tag::Bool &&
                   "verifier invariant violated: JumpIfFalse condition must be bool");
            if (!condition.as_bool()) {
                assert(ins.operand >= 0 &&
                       static_cast<std::size_t>(ins.operand) < function.code.size() &&
                       "verifier invariant violated: JumpIfFalse target must be in range");
                frame.pc = static_cast<std::size_t>(ins.operand);
            } else {
                ++frame.pc;
            }
            break;
        }
        case OpCode::Collect: {
            assert_stack_matches_map(verification, frame);
            heap_.collect();
            assert_stack_matches_map(verification, frame);
            ++frame.pc;
            break;
        }
        case OpCode::Call: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) < module.functions.size() &&
                   "verifier invariant violated: Call target must be in range");
            const auto callee_index = static_cast<std::size_t>(ins.operand);
            const auto& signature = module.functions[callee_index].signature;
            std::vector<Value> arguments(signature.parameters.size(), Value::nil());
            for (std::size_t i = signature.parameters.size(); i > 0; --i) {
                arguments[i - 1] = pop(frame);
            }
            ++frame.pc;
            push_frame(module, callee_index, std::move(arguments));
            break;
        }
        case OpCode::Return: {
            const auto result = pop(frame);
            frames_.pop_back();
            ++instructions_executed_;
            if (frames_.empty()) {
                return result;
            }
            push(frames_.back(), result);
            continue;
        }
        }
        ++instructions_executed_;
    }
    return Value::nil();
}

} // namespace lang
