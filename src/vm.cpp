#include "lang/vm.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace lang {

namespace {

std::string canonical_i64(std::int64_t value) {
    char buffer[32];
    const auto [end, error] =
        std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) {
        throw std::logic_error("canonical i64 formatting failed");
    }
    return std::string(buffer, end);
}

std::int64_t parse_canonical_i64(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        throw std::runtime_error("invalid string for i64 conversion");
    }

    std::size_t index = 0;
    const bool negative = bytes.front() == static_cast<std::uint8_t>('-');
    if (negative) {
        index = 1;
        if (index == bytes.size()) {
            throw std::runtime_error("invalid string for i64 conversion");
        }
    }
    if (bytes[index] == static_cast<std::uint8_t>('0') &&
        (negative || index + 1 != bytes.size())) {
        throw std::runtime_error("invalid string for i64 conversion");
    }

    constexpr std::uint64_t kPositiveLimit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr std::uint64_t kNegativeLimit = kPositiveLimit + 1;
    const auto limit = negative ? kNegativeLimit : kPositiveLimit;
    std::uint64_t magnitude = 0;
    for (; index < bytes.size(); ++index) {
        const auto byte = bytes[index];
        if (byte < static_cast<std::uint8_t>('0') ||
            byte > static_cast<std::uint8_t>('9')) {
            throw std::runtime_error("invalid string for i64 conversion");
        }
        const auto digit = static_cast<std::uint64_t>(byte - '0');
        if (magnitude > (limit - digit) / 10) {
            throw std::runtime_error("invalid string for i64 conversion");
        }
        magnitude = magnitude * 10 + digit;
    }

    if (!negative) {
        return static_cast<std::int64_t>(magnitude);
    }
    if (magnitude == kNegativeLimit) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

std::runtime_error runtime_trap(std::size_t function_index, std::size_t pc,
                                const char* diagnostic) {
    return std::runtime_error("runtime trap at function " +
                              std::to_string(function_index) + " pc " +
                              std::to_string(pc) + ": " + diagnostic);
}

} // namespace

VM::VM() {
    heap_.set_root_provider(this);
}

void VM::set_gc_stress(gc::StressConfig config) {
    for (const auto budget : config.incremental_mark_step_budgets) {
        if (budget == 0) {
            throw std::invalid_argument(
                "incremental marking stress budgets must be positive");
        }
    }
    for (const auto budget : config.incremental_compact_step_budgets) {
        if (budget == 0) {
            throw std::invalid_argument(
                "incremental compaction stress budgets must be positive");
        }
    }
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
        if (frame.closure.has_value()) {
            visitor.visit(*frame.closure);
        }
    }
    if (pending_exception_.has_value()) {
        visitor.visit(*pending_exception_);
    }
}

RuntimeTrace VM::capture_runtime_trace(
    const Module& module, std::size_t active_function_index,
    std::size_t active_pc, RuntimeFailureKind kind,
    std::optional<std::string> exception_variant) const {
    RuntimeTrace trace;
    trace.kind = kind;
    trace.exception_variant = std::move(exception_variant);

    const auto append_frame = [&](std::size_t function_index,
                                  std::size_t pc) {
        RuntimeTraceFrame captured;
        captured.function_index = function_index;
        captured.pc = pc;
        if (function_index < module.functions.size()) {
            const auto& function = module.functions[function_index];
            if (!function.debug_name.empty()) {
                captured.function_name = function.debug_name;
            }
            if (pc < function.source_positions.size()) {
                captured.source_position = function.source_positions[pc];
            }
        }
        trace.frames.push_back(std::move(captured));
    };

    if (frames_.empty()) {
        append_frame(active_function_index, active_pc);
        return trace;
    }

    bool innermost = true;
    for (auto frame = frames_.rbegin(); frame != frames_.rend(); ++frame) {
        if (innermost) {
            append_frame(active_function_index, active_pc);
            innermost = false;
            continue;
        }
        // Call and CallClosure advance a caller to its return continuation
        // before suspension. Diagnostics report the call-site instruction.
        const auto call_site_pc = frame->pc == 0 ? 0 : frame->pc - 1;
        append_frame(frame->function_index, call_site_pc);
    }
    return trace;
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
    assert(map.local_object_slots.size() == frame.locals.size() &&
           "verifier invariant violated: runtime local count differs from stack map");
    for (std::size_t i = 0; i < frame.locals.size(); ++i) {
        if (map.local_object_slots[i]) {
            assert((frame.locals[i].is_object() ||
                    frame.locals[i].tag() == Value::Tag::Nil) &&
                   "verifier invariant violated: runtime local reference slot differs from stack map");
        } else {
            assert(!frame.locals[i].is_object() &&
                   "verifier invariant violated: runtime local object tag differs from stack map");
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

    const bool marking_stress =
        !gc_stress_.incremental_mark_step_budgets.empty();
    const bool compaction_stress =
        !gc_stress_.incremental_compact_step_budgets.empty();
    if (marking_stress &&
        (!compaction_stress || !heap_.incremental_compaction_active())) {
        assert_stack_matches_map(verification, frame);
        if (!heap_.incremental_marking_active()) {
            heap_.start_incremental_marking();
        }
        const auto budget = gc_stress_.incremental_mark_step_budgets[
            incremental_budget_cursor_ %
            gc_stress_.incremental_mark_step_budgets.size()];
        ++incremental_budget_cursor_;
        (void)heap_.incremental_mark_step(budget);
        if (heap_.incremental_marking_quiescent()) {
            if (compaction_stress) {
                heap_.finish_incremental_marking_to_incremental_compaction();
            } else {
                heap_.finish_incremental_marking();
            }
        }
        assert_stack_matches_map(verification, frame);
    } else if (compaction_stress) {
        assert_stack_matches_map(verification, frame);
        if (!heap_.incremental_compaction_active()) {
            heap_.start_incremental_compaction();
        }
        const auto budget = gc_stress_.incremental_compact_step_budgets[
            incremental_compaction_budget_cursor_ %
            gc_stress_.incremental_compact_step_budgets.size()];
        ++incremental_compaction_budget_cursor_;
        (void)heap_.incremental_compact_step(budget);
        if (heap_.incremental_compaction_quiescent()) {
            heap_.finish_incremental_compaction();
        }
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
    output_.clear();
    last_trap_trace_.reset();
    ++raw_function_executions_;
    Module module;
    module.entry_function = 0;
    module.functions.push_back(function);
    return execute_unverified_module(module);
}

void VM::push_frame(const Module& module, std::size_t function_index,
                    std::vector<Value> arguments,
                    std::optional<Value> closure) {
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
    if (closure.has_value()) {
        assert(closure->is_object() &&
               "verifier invariant violated: frame closure must be an object");
        assert(heap_.TEST_ONLY_is_closure(closure->as_object()) &&
               "verifier invariant violated: frame closure object kind mismatch");
        assert(heap_.closure_function_index(closure->as_object()) == function_index &&
               "verifier invariant violated: closure target differs from frame function");
        assert(function.closure_layout.has_value() &&
               *function.closure_layout ==
                   heap_.closure_layout_index(closure->as_object()) &&
               "verifier invariant violated: closure layout differs from frame body");
        frame.closure = *closure;
    }
    frames_.push_back(std::move(frame));
}

void VM::reuse_frame_for_tail_call(const Module& module, Frame& frame,
                                   std::size_t function_index) {
    assert(function_index < module.functions.size() &&
           "verifier invariant violated: TailCall target must be in range");
    const auto& function = module.functions[function_index];
    assert(frame.stack.size() == function.signature.parameters.size() &&
           "verifier invariant violated: TailCall stack must contain only arguments");

    std::vector<Value> arguments(function.signature.parameters.size(),
                                 Value::nil());
    for (std::size_t i = arguments.size(); i > 0; --i) {
        arguments[i - 1] = pop(frame);
    }
    assert(frame.stack.empty() &&
           "verifier invariant violated: TailCall left a stack value");
    assert(function.local_count >= arguments.size() &&
           "verifier invariant violated: TailCall callee locals cannot hold parameters");

    frame.function_index = function_index;
    frame.pc = 0;
    frame.locals.assign(function.local_count, Value::nil());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        frame.locals[i] = arguments[i];
    }
    frame.closure.reset();
}

Value VM::execute(const Module& module) {
    output_.clear();
    last_trap_trace_.reset();
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
    output_.clear();
    last_trap_trace_.reset();
    return execute_verified(module.module(), module.verification());
}

Value VM::execute_verified(const Module& module,
                           const ModuleVerificationResult& verification) {
    if (heap_.incremental_compaction_active()) {
        heap_.finish_incremental_compaction();
    }
    if (heap_.incremental_marking_active()) {
        heap_.finish_incremental_marking();
    }
    frames_.clear();
    pending_exception_.reset();
    last_trap_trace_.reset();
    instructions_executed_ = 0;
    incremental_budget_cursor_ = 0;
    incremental_compaction_budget_cursor_ = 0;

    std::size_t active_function_index = module.entry_function;
    std::size_t active_pc = 0;
    try {
      push_frame(module, module.entry_function, {});
      while (!frames_.empty()) {
        auto& frame = frames_.back();
        active_function_index = frame.function_index;
        active_pc = frame.pc;
        const auto& function = module.functions[frame.function_index];
        if (function.code[frame.pc].op == OpCode::TailCall) {
            // TailCall maps describe a dying-frame boundary. The verifier proves that
            // the operand stack contains exactly the outgoing arguments. Canonicalize
            // every other frame-owned slot before the first map assertion or any
            // instruction-boundary collection can observe it.
            std::fill(frame.locals.begin(), frame.locals.end(), Value::nil());
            frame.closure.reset();
        }
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
        case OpCode::StrIntern: {
            assert(!frame.stack.empty() &&
                   "verifier invariant violated: StrIntern requires an operand");
            const auto source = frame.stack.back();
            assert(source.is_object() &&
                   "verifier invariant violated: StrIntern operand must be object");
            const auto result = heap_.intern_string(source);
            (void)pop(frame);
            push(frame, Value::object(result));
            ++frame.pc;
            break;
        }
        case OpCode::StrSub: {
            assert(frame.stack.size() >= 3 &&
                   "verifier invariant violated: StrSub requires three operands");
            const auto source = frame.stack[frame.stack.size() - 3];
            const auto lo_value = frame.stack[frame.stack.size() - 2];
            const auto hi_value = frame.stack[frame.stack.size() - 1];
            assert(source.is_object() &&
                   "verifier invariant violated: StrSub source must be object");
            assert(lo_value.tag() == Value::Tag::Int64 &&
                   hi_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: StrSub bounds must be i64");
            const auto lo = lo_value.as_i64();
            const auto hi = hi_value.as_i64();
            const auto length = heap_.string_length(source.as_object());
            if (lo < 0 || hi < 0 || lo > hi ||
                static_cast<std::uint64_t>(hi) > length) {
                throw std::out_of_range("string substring bounds out of range");
            }
            const auto result = heap_.allocate_string_substring(
                source, static_cast<std::size_t>(lo),
                static_cast<std::size_t>(hi));
            (void)pop(frame);
            (void)pop(frame);
            (void)pop(frame);
            push(frame, Value::object(result));
            ++frame.pc;
            break;
        }
        case OpCode::StrLt: {
            const auto right = pop(frame);
            const auto left = pop(frame);
            assert(right.is_object() && left.is_object() &&
                   "verifier invariant violated: StrLt operands must be objects");
            const auto left_bytes = heap_.string_bytes(left.as_object());
            const auto right_bytes = heap_.string_bytes(right.as_object());
            push(frame, Value::boolean(std::lexicographical_compare(
                            left_bytes.begin(), left_bytes.end(),
                            right_bytes.begin(), right_bytes.end())));
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
        case OpCode::Print: {
            assert(!frame.stack.empty() && frame.stack.back().is_object() &&
                   "verifier invariant violated: Print operand must be string object");
            const auto bytes = heap_.string_bytes(frame.stack.back().as_object());
            if (output_.size() >= kMaxOutputBytes ||
                bytes.size() > kMaxOutputBytes - output_.size() - 1) {
                throw runtime_trap(frame.function_index, frame.pc,
                                   "output buffer overflow");
            }
            output_.insert(output_.end(), bytes.begin(), bytes.end());
            output_.push_back(static_cast<std::uint8_t>('\n'));
            (void)pop(frame);
            ++frame.pc;
            break;
        }
        case OpCode::I64ToStr: {
            const auto value = pop(frame);
            assert(value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: I64ToStr operand must be i64");
            const auto text = canonical_i64(value.as_i64());
            const auto bytes = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
            push(frame, Value::object(heap_.allocate_string(bytes)));
            ++frame.pc;
            break;
        }
        case OpCode::StrToI64: {
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: StrToI64 operand must be string object");
            try {
                push(frame, Value::int64(
                                parse_canonical_i64(heap_.string_bytes(
                                    receiver.as_object()))));
            } catch (const std::runtime_error&) {
                throw runtime_trap(frame.function_index, frame.pc,
                                   "invalid string for i64 conversion");
            }
            ++frame.pc;
            break;
        }
        case OpCode::BoolToStr: {
            const auto value = pop(frame);
            assert(value.tag() == Value::Tag::Bool &&
                   "verifier invariant violated: BoolToStr operand must be bool");
            const std::string_view text = value.as_bool() ? "true" : "false";
            const auto bytes = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
            push(frame, Value::object(heap_.allocate_string(bytes)));
            ++frame.pc;
            break;
        }
        case OpCode::Nil:
            push(frame, Value::nil());
            ++frame.pc;
            break;
        case OpCode::AllocWeak: {
            const auto target = pop(frame);
            assert(target.is_object() &&
                   "verifier invariant violated: AllocWeak target must be a non-nil object");
            push(frame, Value::object(heap_.allocate_weak(target)));
            ++frame.pc;
            break;
        }
        case OpCode::WeakGet: {
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: WeakGet receiver must be an object");
            assert(heap_.TEST_ONLY_is_weak_ref(receiver.as_object()) &&
                   "verifier invariant violated: WeakGet receiver must be a WeakRef");
            push(frame, heap_.weak_get(receiver.as_object()));
            ++frame.pc;
            break;
        }
        case OpCode::AllocEphemeron: {
            const auto value = pop(frame);
            const auto key = pop(frame);
            const auto layout_index = static_cast<std::size_t>(ins.operand);
            assert(layout_index < module.ephemeron_layouts.size());
            const auto& layout = module.ephemeron_layouts[layout_index];
            push(frame, Value::object(heap_.allocate_ephemeron(
                            key, value, layout.value_is_ref)));
            ++frame.pc;
            break;
        }
        case OpCode::EphemeronKey: {
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   heap_.TEST_ONLY_is_ephemeron(receiver.as_object()));
            push(frame, heap_.ephemeron_key(receiver.as_object()));
            ++frame.pc;
            break;
        }
        case OpCode::EphemeronValue: {
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   heap_.TEST_ONLY_is_ephemeron(receiver.as_object()));
            const auto value = heap_.ephemeron_value(receiver.as_object());
            if (value.tag() == Value::Tag::Nil) {
                throw runtime_trap(frame.function_index, frame.pc,
                                   "ephemeron entry cleared");
            }
            push(frame, value);
            ++frame.pc;
            break;
        }
        case OpCode::EphemeronSetValue: {
            const auto value = pop(frame);
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   heap_.TEST_ONLY_is_ephemeron(receiver.as_object()));
            heap_.ephemeron_set_value(receiver.as_object(), value);
            ++frame.pc;
            break;
        }
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
        case OpCode::I64Abs: {
            const auto value = pop(frame);
            assert(value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: I64Abs operand must be i64");
            const auto integer = value.as_i64();
            if (integer == std::numeric_limits<std::int64_t>::min()) {
                throw runtime_trap(frame.function_index, frame.pc,
                                   "absolute value overflow");
            }
            push(frame, Value::int64(integer < 0 ? -integer : integer));
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
        case OpCode::AllocRecord: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       module.record_layouts.size() &&
                   "verifier invariant violated: AllocRecord layout must be in range");
            const auto layout_index = static_cast<std::size_t>(ins.operand);
            const auto& layout = module.record_layouts[layout_index];
            std::vector<Value> fields(layout.field_types.size(), Value::nil());
            for (std::size_t i = fields.size(); i > 0; --i) {
                fields[i - 1] = pop(frame);
            }
            push(frame, Value::object(heap_.allocate_record(
                            layout_index, std::move(fields),
                            layout.reference_map)));
            ++frame.pc;
            break;
        }
        case OpCode::RecordGet: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       module.record_layouts.size() &&
                   "verifier invariant violated: RecordGet layout must be in range");
            const auto layout_index = static_cast<std::size_t>(ins.operand);
            const auto& layout = module.record_layouts[layout_index];
            assert(ins.operand2 >= 0 &&
                   static_cast<std::size_t>(ins.operand2) <
                       layout.field_types.size() &&
                   "verifier invariant violated: RecordGet field must be in range");
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: RecordGet receiver must be object");
            assert(heap_.TEST_ONLY_is_record(receiver.as_object()) &&
                   "verifier invariant violated: RecordGet receiver must be record");
            assert(heap_.record_layout_index(receiver.as_object()) == layout_index &&
                   "verifier invariant violated: RecordGet receiver layout mismatch");
            push(frame, heap_.record_get(
                            receiver.as_object(),
                            static_cast<std::size_t>(ins.operand2)));
            ++frame.pc;
            break;
        }
        case OpCode::RecordSet: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       module.record_layouts.size() &&
                   "verifier invariant violated: RecordSet layout must be in range");
            const auto layout_index = static_cast<std::size_t>(ins.operand);
            const auto& layout = module.record_layouts[layout_index];
            assert(ins.operand2 >= 0 &&
                   static_cast<std::size_t>(ins.operand2) <
                       layout.field_types.size() &&
                   "verifier invariant violated: RecordSet field must be in range");
            const auto stored = pop(frame);
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: RecordSet receiver must be object");
            assert(heap_.TEST_ONLY_is_record(receiver.as_object()) &&
                   "verifier invariant violated: RecordSet receiver must be record");
            assert(heap_.record_layout_index(receiver.as_object()) == layout_index &&
                   "verifier invariant violated: RecordSet receiver layout mismatch");
            heap_.record_set(receiver.as_object(),
                             static_cast<std::size_t>(ins.operand2), stored);
            ++frame.pc;
            break;
        }
        case OpCode::AllocVariant: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       module.variant_layouts.size() &&
                   "verifier invariant violated: AllocVariant layout must be in range");
            const auto layout_index = static_cast<std::size_t>(ins.operand);
            const auto& layout = module.variant_layouts[layout_index];
            assert(ins.operand2 >= 0 &&
                   static_cast<std::size_t>(ins.operand2) <
                       layout.cases.size() &&
                   "verifier invariant violated: AllocVariant case must be in range");
            const auto case_index = static_cast<std::size_t>(ins.operand2);
            const auto& case_layout = layout.cases[case_index];
            std::vector<Value> fields(case_layout.field_types.size(),
                                      Value::nil());
            for (std::size_t i = fields.size(); i > 0; --i) {
                fields[i - 1] = pop(frame);
            }
            std::vector<std::vector<bool>> case_reference_maps;
            case_reference_maps.reserve(layout.cases.size());
            for (const auto& variant_case : layout.cases) {
                case_reference_maps.push_back(variant_case.reference_map);
            }
            push(frame, Value::object(heap_.allocate_variant(
                            layout_index, case_index, std::move(fields),
                            std::move(case_reference_maps))));
            ++frame.pc;
            break;
        }
        case OpCode::VariantTag: {
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: VariantTag receiver must be object");
            const auto object = receiver.as_object();
            assert(heap_.TEST_ONLY_is_variant(object) &&
                   "verifier invariant violated: VariantTag receiver must be variant");
            const auto layout_index = heap_.variant_layout_index(object);
            assert(layout_index < module.variant_layouts.size() &&
                   "verifier invariant violated: VariantTag layout must be in range");
            const auto tag = heap_.variant_tag(object);
            assert(tag < module.variant_layouts[layout_index].cases.size() &&
                   "verifier invariant violated: VariantTag tag must be in range");
            assert(tag <= static_cast<std::size_t>(
                              std::numeric_limits<std::int64_t>::max()) &&
                   "validated variant tag must fit in i64");
            push(frame, Value::int64(static_cast<std::int64_t>(tag)));
            ++frame.pc;
            break;
        }
        case OpCode::VariantGet: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       module.variant_layouts.size() &&
                   "verifier invariant violated: VariantGet layout must be in range");
            const auto layout_index = static_cast<std::size_t>(ins.operand);
            const auto& layout = module.variant_layouts[layout_index];
            assert(ins.operand2 >= 0 &&
                   static_cast<std::size_t>(ins.operand2) <
                       layout.cases.size() &&
                   "verifier invariant violated: VariantGet case must be in range");
            const auto case_index = static_cast<std::size_t>(ins.operand2);
            const auto& case_layout = layout.cases[case_index];
            assert(ins.operand3 >= 0 &&
                   static_cast<std::size_t>(ins.operand3) <
                       case_layout.field_types.size() &&
                   "verifier invariant violated: VariantGet field must be in range");
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: VariantGet receiver must be object");
            const auto object = receiver.as_object();
            assert(heap_.TEST_ONLY_is_variant(object) &&
                   "verifier invariant violated: VariantGet receiver must be variant");
            assert(heap_.variant_layout_index(object) == layout_index &&
                   "verifier invariant violated: VariantGet receiver layout mismatch");
            if (heap_.variant_tag(object) != case_index) {
                throw std::runtime_error("variant case tag mismatch");
            }
            push(frame, heap_.variant_get(
                            object,
                            static_cast<std::size_t>(ins.operand3)));
            ++frame.pc;
            break;
        }
        case OpCode::TryBegin:
        case OpCode::TryEnd:
            ++frame.pc;
            break;
        case OpCode::Throw: {
            pending_exception_ = pop(frame);
            assert(pending_exception_->is_object() &&
                   heap_.TEST_ONLY_is_variant(pending_exception_->as_object()) &&
                   "verifier invariant violated: thrown value must be a variant");
            // Unwind removes frame objects. Snapshot only copied diagnostic data now,
            // then publish it only if no language handler accepts the exception.
            auto uncaught_trace = capture_runtime_trace(
                module, frame.function_index, frame.pc,
                RuntimeFailureKind::UncaughtException);
            bool initial = true;
            bool handled = false;
            while (!frames_.empty()) {
                auto& unwind_frame = frames_.back();
                const auto& unwind_function = module.functions[unwind_frame.function_index];
                const auto site = initial ? unwind_frame.pc
                                          : (unwind_frame.pc == 0 ? 0 : unwind_frame.pc - 1);
                const auto layout = heap_.variant_layout_index(
                    pending_exception_->as_object());
                const ExceptionHandler* selected = nullptr;
                for (const auto& handler : unwind_function.exception_handlers) {
                    if (handler.try_begin < site && site < handler.try_end &&
                        handler.variant_layout == layout &&
                        (selected == nullptr || handler.try_begin > selected->try_begin)) {
                        selected = &handler;
                    }
                }
                if (selected != nullptr) {
                    unwind_frame.stack.clear();
                    unwind_frame.pc = selected->target;
                    push(unwind_frame, *pending_exception_);
                    pending_exception_.reset();
                    handled = true;
                    break;
                }
                if (gc_stress_.collect_every_n_instructions == 1) {
                    heap_.collect();
                }
                frames_.pop_back();
                initial = false;
            }
            if (!handled) {
                const auto layout = heap_.variant_layout_index(
                    pending_exception_->as_object());
                const auto name = layout < module.variant_layouts.size()
                                      ? module.variant_layouts[layout].name
                                      : std::string("<invalid>");
                uncaught_trace.exception_variant = name;
                last_trap_trace_ = std::move(uncaught_trace);
                pending_exception_.reset();
                throw std::runtime_error("uncaught exception " + name);
            }
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
        case OpCode::AllocMap: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       module.map_layouts.size() &&
                   "verifier invariant violated: AllocMap layout must be in range");
            const auto layout_index = static_cast<std::size_t>(ins.operand);
            const auto& layout = module.map_layouts[layout_index];
            push(frame, Value::object(heap_.allocate_map(
                            layout_index, layout.key_is_ref,
                            layout.value_is_ref)));
            ++frame.pc;
            break;
        }
        case OpCode::MapSet: {
            const auto stored_value = pop(frame);
            const auto key = pop(frame);
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: MapSet receiver must be object");
            assert(heap_.TEST_ONLY_is_map(receiver.as_object()) &&
                   "verifier invariant violated: MapSet receiver must be map");
            heap_.map_set(receiver.as_object(), key, stored_value);
            ++frame.pc;
            break;
        }
        case OpCode::MapGet: {
            const auto key = pop(frame);
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: MapGet receiver must be object");
            assert(heap_.TEST_ONLY_is_map(receiver.as_object()) &&
                   "verifier invariant violated: MapGet receiver must be map");
            push(frame, heap_.map_get(receiver.as_object(), key));
            ++frame.pc;
            break;
        }
        case OpCode::MapHas: {
            const auto key = pop(frame);
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: MapHas receiver must be object");
            assert(heap_.TEST_ONLY_is_map(receiver.as_object()) &&
                   "verifier invariant violated: MapHas receiver must be map");
            push(frame, Value::boolean(
                            heap_.map_has(receiver.as_object(), key)));
            ++frame.pc;
            break;
        }
        case OpCode::MapLen: {
            const auto receiver = pop(frame);
            assert(receiver.is_object() &&
                   "verifier invariant violated: MapLen receiver must be object");
            assert(heap_.TEST_ONLY_is_map(receiver.as_object()) &&
                   "verifier invariant violated: MapLen receiver must be map");
            const auto length = heap_.map_length(receiver.as_object());
            if (length > static_cast<std::size_t>(
                             std::numeric_limits<std::int64_t>::max())) {
                throw std::length_error("map length exceeds i64 result range");
            }
            push(frame, Value::int64(static_cast<std::int64_t>(length)));
            ++frame.pc;
            break;
        }
        case OpCode::MapKeyAt:
        case OpCode::MapValueAt: {
            const auto index_value = pop(frame);
            const auto receiver = pop(frame);
            assert(index_value.tag() == Value::Tag::Int64 &&
                   "verifier invariant violated: map positional index must be i64");
            assert(receiver.is_object() &&
                   "verifier invariant violated: map positional receiver must be object");
            assert(heap_.TEST_ONLY_is_map(receiver.as_object()) &&
                   "verifier invariant violated: map positional receiver must be map");
            const auto index = index_value.as_i64();
            if (index < 0) {
                throw std::out_of_range("map entry index out of bounds");
            }
            push(frame, ins.op == OpCode::MapKeyAt
                            ? heap_.map_key_at(receiver.as_object(),
                                               static_cast<std::size_t>(index))
                            : heap_.map_value_at(receiver.as_object(),
                                                 static_cast<std::size_t>(index)));
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
        case OpCode::AllocClosure: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       module.closure_layouts.size() &&
                   "verifier invariant violated: AllocClosure layout must be in range");
            const auto layout_index = static_cast<std::size_t>(ins.operand);
            const auto& layout = module.closure_layouts[layout_index];
            std::vector<Value> captures(layout.capture_types.size(), Value::nil());
            for (std::size_t i = captures.size(); i > 0; --i) {
                captures[i - 1] = pop(frame);
            }
            const auto closure = heap_.allocate_closure(
                layout_index, layout.function_index, std::move(captures),
                layout.capture_map);
            push(frame, Value::object(closure));
            ++frame.pc;
            break;
        }
        case OpCode::CallClosure: {
            const auto closure = pop(frame);
            assert(closure.is_object() &&
                   "verifier invariant violated: CallClosure callee must be object");
            assert(heap_.TEST_ONLY_is_closure(closure.as_object()) &&
                   "verifier invariant violated: CallClosure object must be closure");
            const auto callee_index =
                heap_.closure_function_index(closure.as_object());
            assert(callee_index < module.functions.size() &&
                   "validated closure target must be in module function table");
            const auto& signature = module.functions[callee_index].signature;
            std::vector<Value> arguments(signature.parameters.size(), Value::nil());
            for (std::size_t i = arguments.size(); i > 0; --i) {
                arguments[i - 1] = pop(frame);
            }
            ++frame.pc;
            push_frame(module, callee_index, std::move(arguments), closure);
            break;
        }
        case OpCode::LoadCapture: {
            assert(frame.closure.has_value() &&
                   "verifier invariant violated: LoadCapture requires closure frame");
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       heap_.closure_capture_count(frame.closure->as_object()) &&
                   "verifier invariant violated: LoadCapture index must be in range");
            push(frame, heap_.closure_capture(
                            frame.closure->as_object(),
                            static_cast<std::size_t>(ins.operand)));
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
        case OpCode::TailCall: {
            assert(ins.operand >= 0 &&
                   static_cast<std::size_t>(ins.operand) <
                       module.functions.size() &&
                   "verifier invariant violated: TailCall target must be in range");
            reuse_frame_for_tail_call(
                module, frame, static_cast<std::size_t>(ins.operand));
            break;
        }
        case OpCode::Return: {
            if (heap_.incremental_marking_active()) {
                if (!gc_stress_.incremental_compact_step_budgets.empty()) {
                    heap_.finish_incremental_marking_to_incremental_compaction();
                } else {
                    heap_.finish_incremental_marking();
                }
            }
            if (heap_.incremental_compaction_active()) {
                heap_.finish_incremental_compaction();
            }
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
    } catch (...) {
        if (!last_trap_trace_.has_value()) {
            last_trap_trace_ = capture_runtime_trace(
                module, active_function_index, active_pc,
                RuntimeFailureKind::Trap);
        }
        if (heap_.incremental_marking_active()) {
            heap_.finish_incremental_marking();
        }
        if (heap_.incremental_compaction_active()) {
            heap_.finish_incremental_compaction();
        }
        throw;
    }
}

} // namespace lang
