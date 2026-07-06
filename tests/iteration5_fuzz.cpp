#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kSnapshotSeed = 17;
constexpr std::uint64_t kCallSnapshotSeed = 17;
constexpr std::uint64_t kFirstCorpusSeed = 1;
constexpr std::uint64_t kCorpusSize = 64;

enum class Grammar {
    Single,
    Calls,
};

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

const char* value_kind_name(lang::ValueKind kind) {
    switch (kind) {
    case lang::ValueKind::Int64:
        return "i64";
    case lang::ValueKind::Bool:
        return "bool";
    case lang::ValueKind::Object:
        return "object";
    case lang::ValueKind::Nil:
        return "nil";
    }
    return "<unknown>";
}

std::string signature_text(const lang::FunctionSignature& signature) {
    std::ostringstream out;
    out << "(";
    for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << value_kind_name(signature.parameters[i]);
    }
    out << ")->" << value_kind_name(signature.return_type);
    return out.str();
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

std::string describe(const lang::Module& module) {
    std::ostringstream out;
    out << "entry=" << module.entry_function << " functions="
        << module.functions.size() << "\n";
    for (std::size_t function_index = 0; function_index < module.functions.size();
         ++function_index) {
        const auto& function = module.functions[function_index];
        out << "function=" << function_index << " signature="
            << signature_text(function.signature) << " locals="
            << function.local_count << "\n";
        for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
            const auto& ins = function.code[pc];
            out << "  #" << pc << " " << op_name(ins.op) << " " << ins.operand
                << "\n";
        }
    }
    return out.str();
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E37'79B9'7F4A'7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBull;
        return z ^ (z >> 31);
    }

    std::uint64_t bounded(std::uint64_t exclusive_max) {
        assert(exclusive_max > 0);
        return next() % exclusive_max;
    }

    std::int64_t small_i64() {
        return static_cast<std::int64_t>(bounded(81)) - 40;
    }

private:
    std::uint64_t state_;
};

enum class Kind {
    Int64,
    Bool,
    Object,
};

Kind kind_from_value_kind(lang::ValueKind kind) {
    switch (kind) {
    case lang::ValueKind::Int64:
        return Kind::Int64;
    case lang::ValueKind::Bool:
        return Kind::Bool;
    case lang::ValueKind::Object:
        return Kind::Object;
    case lang::ValueKind::Nil:
        break;
    }
    throw std::logic_error("fuzzer generator does not emit nil values");
}

lang::FunctionSignature object_return_signature() {
    lang::FunctionSignature signature;
    signature.return_type = lang::ValueKind::Object;
    return signature;
}

class Builder {
public:
    explicit Builder(std::uint32_t local_count)
        : Builder(local_count, object_return_signature()) {}

    Builder(std::uint32_t local_count, lang::FunctionSignature signature) {
        function_.local_count = local_count;
        function_.signature = std::move(signature);
        assert(function_.local_count >= function_.signature.parameters.size());
        locals_.resize(local_count);
        for (std::size_t i = 0; i < function_.signature.parameters.size(); ++i) {
            locals_[i] = kind_from_value_kind(function_.signature.parameters[i]);
        }
    }

    [[nodiscard]] std::size_t pc() const { return function_.code.size(); }
    [[nodiscard]] const std::vector<std::optional<Kind>>& locals() const { return locals_; }
    [[nodiscard]] const std::vector<Kind>& stack() const { return stack_; }

    void restore_state(std::vector<Kind> stack,
                       std::vector<std::optional<Kind>> locals) {
        stack_ = std::move(stack);
        locals_ = std::move(locals);
    }

    void constant_i64(std::int64_t value) {
        emit(lang::OpCode::ConstantI64, value);
        stack_.push_back(Kind::Int64);
    }

    void add_i64() {
        pop_expect(Kind::Int64);
        pop_expect(Kind::Int64);
        emit(lang::OpCode::AddI64, 0);
        stack_.push_back(Kind::Int64);
    }

    void less_i64() {
        pop_expect(Kind::Int64);
        pop_expect(Kind::Int64);
        emit(lang::OpCode::LessI64, 0);
        stack_.push_back(Kind::Bool);
    }

    void alloc_pair() {
        pop_any();
        pop_any();
        emit(lang::OpCode::AllocPair, 0);
        stack_.push_back(Kind::Object);
    }

    void load_local(std::uint32_t local) {
        assert(local < locals_.size());
        assert(locals_[local].has_value());
        emit(lang::OpCode::LoadLocal, local);
        stack_.push_back(*locals_[local]);
    }

    void store_local(std::uint32_t local) {
        assert(local < locals_.size());
        const auto value = pop_any();
        emit(lang::OpCode::StoreLocal, local);
        locals_[local] = value;
    }

    void get_left_object() {
        pop_expect(Kind::Object);
        emit(lang::OpCode::GetLeft, 0);
        stack_.push_back(Kind::Object);
    }

    void get_right_object() {
        pop_expect(Kind::Object);
        emit(lang::OpCode::GetRight, 0);
        stack_.push_back(Kind::Object);
    }

    void set_left() {
        pop_any();
        pop_expect(Kind::Object);
        emit(lang::OpCode::SetLeft, 0);
    }

    void set_right() {
        pop_any();
        pop_expect(Kind::Object);
        emit(lang::OpCode::SetRight, 0);
    }

    void collect() { emit(lang::OpCode::Collect, 0); }

    void call(std::size_t callee_index, const lang::FunctionSignature& signature) {
        for (std::size_t i = signature.parameters.size(); i > 0; --i) {
            pop_expect(kind_from_value_kind(signature.parameters[i - 1]));
        }
        emit(lang::OpCode::Call, static_cast<std::int64_t>(callee_index));
        stack_.push_back(kind_from_value_kind(signature.return_type));
    }

    std::size_t jump_if_false_placeholder() {
        pop_expect(Kind::Bool);
        const auto index = pc();
        emit(lang::OpCode::JumpIfFalse, -1);
        return index;
    }

    void jump(std::size_t target) {
        emit(lang::OpCode::Jump, static_cast<std::int64_t>(target));
    }

    void patch_jump_target(std::size_t instruction_index, std::size_t target) {
        assert(instruction_index < function_.code.size());
        function_.code[instruction_index].operand = static_cast<std::int64_t>(target);
    }

    void return_top() {
        pop_expect(kind_from_value_kind(function_.signature.return_type));
        emit(lang::OpCode::Return, 0);
    }

    lang::Function finish() const { return function_; }

private:
    void emit(lang::OpCode op, std::int64_t operand) {
        function_.code.push_back(lang::Instruction{op, operand});
    }

    Kind pop_any() {
        assert(!stack_.empty());
        const auto kind = stack_.back();
        stack_.pop_back();
        return kind;
    }

    void pop_expect(Kind expected) {
        const auto actual = pop_any();
        assert(actual == expected);
    }

    lang::Function function_;
    std::vector<Kind> stack_;
    std::vector<std::optional<Kind>> locals_;
};

void require_backedge_compatible(
    const std::vector<std::optional<Kind>>& loop_entry,
    const std::vector<std::optional<Kind>>& backedge,
    const std::string& context) {
    require(loop_entry.size() == backedge.size(), context + ": local-count mismatch");
    for (std::size_t i = 0; i < loop_entry.size(); ++i) {
        if (!loop_entry[i].has_value()) {
            continue;
        }
        require(backedge[i].has_value() && *backedge[i] == *loop_entry[i],
                context + ": loop-carried local " + std::to_string(i) +
                    " changed kind");
    }
}

lang::Function generate_program(std::uint64_t seed) {
    SplitMix64 rng(seed);

    constexpr std::uint32_t kHead = 0;
    constexpr std::uint32_t kI = 1;
    constexpr std::uint32_t kOld = 2;
    constexpr std::uint32_t kShared = 3;
    constexpr std::uint32_t kChild = 4;
    constexpr std::uint32_t kScratch = 5;
    constexpr std::uint32_t kFiller = 6;
    constexpr std::uint32_t kScratch2 = 7;
    constexpr std::uint32_t kLocalCount = 8;

    const auto loop_limit = static_cast<std::int64_t>(3 + rng.bounded(4));
    const bool mutate_old_left = rng.bounded(2) == 0;
    const bool collect_inside_loop = rng.bounded(3) != 0;
    const bool collect_after_increment = rng.bounded(2) == 0;
    const auto return_selector = rng.bounded(4);

    Builder b(kLocalCount);

    b.constant_i64(rng.small_i64());
    b.constant_i64(rng.small_i64());
    b.alloc_pair();
    b.store_local(kShared);

    b.load_local(kShared);
    b.load_local(kShared);
    b.alloc_pair();
    b.store_local(kOld);

    b.load_local(kOld);
    b.store_local(kHead);

    b.collect();

    b.constant_i64(0);
    b.store_local(kI);

    b.load_local(kHead);
    b.store_local(kScratch);
    b.load_local(kShared);
    b.store_local(kScratch2);

    b.constant_i64(0);
    b.store_local(kFiller);

    const auto loop_header = b.pc();
    const auto loop_entry_stack = b.stack();
    const auto loop_entry_locals = b.locals();

    b.load_local(kI);
    b.constant_i64(loop_limit);
    b.less_i64();
    const auto exit_jump = b.jump_if_false_placeholder();

    b.constant_i64(rng.small_i64());
    b.load_local(kI);
    b.alloc_pair();
    b.store_local(kFiller);

    b.load_local(kShared);
    b.load_local(kHead);
    b.alloc_pair();
    b.store_local(kChild);

    b.load_local(kChild);
    b.load_local(kChild);
    b.set_left();

    b.load_local(kOld);
    b.load_local(kChild);
    if (mutate_old_left) {
        b.set_left();
    } else {
        b.set_right();
    }

    b.load_local(kChild);
    b.store_local(kHead);

    b.load_local(kHead);
    b.get_left_object();
    b.store_local(kScratch);

    b.load_local(kHead);
    b.get_right_object();
    b.store_local(kScratch2);

    b.constant_i64(0);
    b.store_local(kFiller);

    if (collect_inside_loop) {
        b.collect();
    }

    b.load_local(kI);
    b.constant_i64(1);
    b.add_i64();
    b.store_local(kI);

    if (collect_after_increment) {
        b.collect();
    }

    require(b.stack().empty(), "generator bug: loop body leaves stack values");
    require_backedge_compatible(loop_entry_locals, b.locals(), "generator bug");
    b.jump(loop_header);

    const auto exit_pc = b.pc();
    b.patch_jump_target(exit_jump, exit_pc);
    b.restore_state(loop_entry_stack, loop_entry_locals);

    switch (return_selector) {
    case 0:
        b.load_local(kHead);
        break;
    case 1:
        b.load_local(kOld);
        break;
    case 2:
        b.load_local(kOld);
        if (mutate_old_left) {
            b.get_left_object();
        } else {
            b.get_right_object();
        }
        break;
    default:
        b.load_local(kHead);
        b.get_right_object();
        break;
    }
    b.return_top();

    auto function = b.finish();
    require(lang::verify(function),
            "generator emitted verifier-rejected function for seed " +
                std::to_string(seed) + "\n" + describe(function));
    return function;
}

lang::FunctionSignature make_signature(std::vector<lang::ValueKind> parameters,
                                       lang::ValueKind return_type) {
    lang::FunctionSignature signature;
    signature.parameters = std::move(parameters);
    signature.return_type = return_type;
    return signature;
}

std::vector<lang::FunctionSignature> call_signatures(std::size_t function_count) {
    std::vector<lang::FunctionSignature> signatures(function_count);
    signatures[0] = make_signature({}, lang::ValueKind::Object);
    signatures[1] = make_signature({lang::ValueKind::Int64,
                                    lang::ValueKind::Object,
                                    lang::ValueKind::Object},
                                   lang::ValueKind::Object);
    if (function_count >= 3) {
        signatures[2] = make_signature({lang::ValueKind::Object,
                                        lang::ValueKind::Object,
                                        lang::ValueKind::Bool,
                                        lang::ValueKind::Int64},
                                       lang::ValueKind::Object);
    }
    if (function_count >= 4) {
        signatures[3] = make_signature({lang::ValueKind::Int64,
                                        lang::ValueKind::Int64,
                                        lang::ValueKind::Object},
                                       lang::ValueKind::Bool);
    }
    if (function_count >= 5) {
        signatures[4] = make_signature({lang::ValueKind::Int64,
                                        lang::ValueKind::Bool,
                                        lang::ValueKind::Object},
                                       lang::ValueKind::Int64);
    }
    return signatures;
}

lang::Function generate_call_entry(SplitMix64& rng,
                                   const std::vector<lang::FunctionSignature>& signatures) {
    constexpr std::uint32_t kRoot = 0;
    constexpr std::uint32_t kTail = 1;
    constexpr std::uint32_t kReturned = 2;
    constexpr std::uint32_t kFlag = 3;
    constexpr std::uint32_t kDepth = 4;
    constexpr std::uint32_t kScratch = 5;
    constexpr std::uint32_t kDead = 6;
    constexpr std::uint32_t kLocalCount = 7;

    const auto base_depth = static_cast<std::int64_t>(2 + rng.bounded(4));
    const bool final_mutate_left = rng.bounded(2) == 0;

    Builder b(kLocalCount, signatures[0]);
    b.constant_i64(rng.small_i64());
    b.constant_i64(rng.small_i64());
    b.alloc_pair();
    b.store_local(kTail);

    b.load_local(kTail);
    b.constant_i64(rng.small_i64());
    b.alloc_pair();
    b.store_local(kRoot);

    b.load_local(kRoot);
    b.store_local(kScratch);

    b.collect();

    b.constant_i64(rng.small_i64());
    b.constant_i64(rng.small_i64());
    b.alloc_pair();
    b.store_local(kDead);
    b.constant_i64(0);
    b.store_local(kDead);

    if (signatures.size() >= 4) {
        b.load_local(kTail);
        b.constant_i64(rng.small_i64());
        b.constant_i64(rng.small_i64());
        b.load_local(kRoot);
        b.call(3, signatures[3]);
        b.store_local(kFlag);
        b.store_local(kScratch);
    } else {
        b.constant_i64(rng.small_i64());
        b.constant_i64(rng.small_i64());
        b.less_i64();
        b.store_local(kFlag);
    }

    if (signatures.size() >= 5) {
        b.load_local(kTail);
        b.constant_i64(base_depth);
        b.load_local(kFlag);
        b.load_local(kRoot);
        b.call(4, signatures[4]);
        b.store_local(kDepth);
        b.store_local(kScratch);
    } else {
        b.constant_i64(base_depth);
        b.store_local(kDepth);
    }

    b.load_local(kRoot);
    b.load_local(kDepth);
    b.load_local(kTail);
    b.load_local(kRoot);
    b.call(1, signatures[1]);
    b.store_local(kReturned);
    b.store_local(kScratch);

    if (signatures.size() >= 3) {
        b.load_local(kTail);
        b.load_local(kRoot);
        b.load_local(kReturned);
        b.load_local(kFlag);
        b.load_local(kDepth);
        b.call(2, signatures[2]);
        b.store_local(kReturned);
        b.store_local(kScratch);
    }

    b.load_local(kRoot);
    b.load_local(kReturned);
    if (final_mutate_left) {
        b.set_left();
    } else {
        b.set_right();
    }
    b.collect();
    b.load_local(kRoot);
    b.return_top();
    return b.finish();
}

lang::Function generate_recursive_builder(
    SplitMix64& rng, const std::vector<lang::FunctionSignature>& signatures) {
    constexpr std::uint32_t kCounter = 0;
    constexpr std::uint32_t kTail = 1;
    constexpr std::uint32_t kAnchor = 2;
    constexpr std::uint32_t kNode = 3;
    constexpr std::uint32_t kLocalCount = 4;

    const bool mutate_anchor_left = rng.bounded(2) == 0;

    Builder b(kLocalCount, signatures[1]);
    b.load_local(kCounter);
    b.constant_i64(1);
    b.less_i64();
    const auto recurse_jump = b.jump_if_false_placeholder();
    b.load_local(kTail);
    b.return_top();

    const auto recurse_pc = b.pc();
    b.patch_jump_target(recurse_jump, recurse_pc);
    b.load_local(kCounter);
    b.load_local(kTail);
    b.alloc_pair();
    b.store_local(kNode);

    b.load_local(kNode);
    b.load_local(kNode);
    b.set_right();

    b.load_local(kAnchor);
    b.load_local(kNode);
    if (mutate_anchor_left) {
        b.set_left();
    } else {
        b.set_right();
    }
    b.collect();

    b.load_local(kCounter);
    b.constant_i64(-1);
    b.add_i64();
    b.load_local(kNode);
    b.load_local(kAnchor);
    b.call(1, signatures[1]);
    b.return_top();
    return b.finish();
}

lang::Function generate_object_mutator(
    SplitMix64& rng, const std::vector<lang::FunctionSignature>& signatures) {
    constexpr std::uint32_t kAnchor = 0;
    constexpr std::uint32_t kIncoming = 1;
    constexpr std::uint32_t kFlag = 2;
    constexpr std::uint32_t kSalt = 3;
    constexpr std::uint32_t kScratch = 4;
    constexpr std::uint32_t kLocalCount = 5;

    const bool self_cycle_left = rng.bounded(2) == 0;

    Builder b(kLocalCount, signatures[2]);
    b.load_local(kIncoming);
    b.load_local(kSalt);
    b.alloc_pair();
    b.store_local(kScratch);

    b.load_local(kScratch);
    b.load_local(kScratch);
    if (self_cycle_left) {
        b.set_left();
    } else {
        b.set_right();
    }

    b.load_local(kFlag);
    const auto else_jump = b.jump_if_false_placeholder();
    b.load_local(kAnchor);
    b.load_local(kScratch);
    b.set_left();
    b.collect();
    b.load_local(kScratch);
    b.return_top();

    const auto else_pc = b.pc();
    b.patch_jump_target(else_jump, else_pc);
    b.load_local(kAnchor);
    b.load_local(kScratch);
    b.set_right();
    b.collect();
    b.load_local(kScratch);
    b.return_top();
    return b.finish();
}

lang::Function generate_bool_helper(
    const std::vector<lang::FunctionSignature>& signatures) {
    constexpr std::uint32_t kLhs = 0;
    constexpr std::uint32_t kRhs = 1;
    constexpr std::uint32_t kAnchor = 2;
    constexpr std::uint32_t kScratch = 3;
    constexpr std::uint32_t kLocalCount = 4;

    Builder b(kLocalCount, signatures[3]);
    b.load_local(kLhs);
    b.load_local(kAnchor);
    b.alloc_pair();
    b.store_local(kScratch);
    b.load_local(kAnchor);
    b.load_local(kScratch);
    b.set_right();
    b.collect();
    b.load_local(kLhs);
    b.load_local(kRhs);
    b.less_i64();
    b.return_top();
    return b.finish();
}

lang::Function generate_i64_helper(
    SplitMix64& rng, const std::vector<lang::FunctionSignature>& signatures) {
    constexpr std::uint32_t kBase = 0;
    constexpr std::uint32_t kFlag = 1;
    constexpr std::uint32_t kAnchor = 2;
    constexpr std::uint32_t kScratch = 3;
    constexpr std::uint32_t kLocalCount = 4;

    const bool mutate_anchor_left = rng.bounded(2) == 0;

    Builder b(kLocalCount, signatures[4]);
    b.load_local(kBase);
    b.load_local(kAnchor);
    b.alloc_pair();
    b.store_local(kScratch);
    b.load_local(kScratch);
    b.load_local(kScratch);
    b.set_right();
    b.load_local(kAnchor);
    b.load_local(kScratch);
    if (mutate_anchor_left) {
        b.set_left();
    } else {
        b.set_right();
    }
    b.collect();

    b.load_local(kFlag);
    const auto else_jump = b.jump_if_false_placeholder();
    b.load_local(kBase);
    b.constant_i64(1);
    b.add_i64();
    b.return_top();

    const auto else_pc = b.pc();
    b.patch_jump_target(else_jump, else_pc);
    b.load_local(kBase);
    b.constant_i64(2);
    b.add_i64();
    b.return_top();
    return b.finish();
}

lang::Module generate_call_module(std::uint64_t seed) {
    SplitMix64 rng(seed ^ 0xC411'5EED'F00D'0008ull);
    const auto function_count =
        static_cast<std::size_t>(2 + ((seed + 2) % 4));
    const auto signatures = call_signatures(function_count);

    lang::Module module;
    module.entry_function = 0;
    module.functions.reserve(function_count);
    module.functions.push_back(generate_call_entry(rng, signatures));
    module.functions.push_back(generate_recursive_builder(rng, signatures));
    if (function_count >= 3) {
        module.functions.push_back(generate_object_mutator(rng, signatures));
    }
    if (function_count >= 4) {
        module.functions.push_back(generate_bool_helper(signatures));
    }
    if (function_count >= 5) {
        module.functions.push_back(generate_i64_helper(rng, signatures));
    }

    require(lang::verify(module),
            "call generator emitted verifier-rejected module for seed " +
                std::to_string(seed) + "\n" + describe(module));
    return module;
}

struct Schedule {
    const char* name;
    lang::gc::StressConfig stress;
};

std::vector<Schedule> schedules() {
    std::vector<Schedule> result;
    result.push_back({"no_stress", {}});

    lang::gc::StressConfig before_alloc;
    before_alloc.collect_before_every_allocation = true;
    result.push_back({"before_every_alloc", before_alloc});

    lang::gc::StressConfig after_alloc;
    after_alloc.collect_after_every_allocation = true;
    result.push_back({"after_every_alloc", after_alloc});

    for (const auto n : {1ull, 3ull, 7ull}) {
        lang::gc::StressConfig major;
        major.collect_every_n_instructions = n;
        result.push_back({n == 1 ? "major_every_1"
                                 : (n == 3 ? "major_every_3" : "major_every_7"),
                          major});
    }

    for (const auto n : {1ull, 4ull}) {
        lang::gc::StressConfig minor;
        minor.collect_minor_every_n_instructions = n;
        result.push_back({n == 1 ? "minor_every_1" : "minor_every_4", minor});
    }

    lang::gc::StressConfig after_barrier;
    after_barrier.collect_minor_after_every_write_barrier = true;
    result.push_back({"minor_after_every_barrier", after_barrier});

    lang::gc::StressConfig combined;
    combined.collect_before_every_allocation = true;
    combined.collect_after_every_allocation = true;
    combined.collect_every_n_instructions = 7;
    combined.collect_minor_every_n_instructions = 4;
    combined.collect_minor_after_every_write_barrier = true;
    result.push_back({"combined", combined});

    return result;
}

const Schedule& find_schedule(const std::vector<Schedule>& all, const std::string& name) {
    for (const auto& schedule : all) {
        if (schedule.name == name) {
            return schedule;
        }
    }
    std::ostringstream out;
    out << "unknown schedule '" << name << "'. valid schedules:";
    for (const auto& schedule : all) {
        out << " " << schedule.name;
    }
    throw std::runtime_error(out.str());
}

std::string value_token(const lang::gc::Heap& heap, lang::Value value,
                        std::map<lang::ObjectId, std::size_t>& indexes,
                        std::vector<lang::ObjectId>& order) {
    std::ostringstream out;
    switch (value.tag()) {
    case lang::Value::Tag::Int64:
        out << "i64(" << value.as_i64() << ")";
        return out.str();
    case lang::Value::Tag::Bool:
        out << "bool(" << (value.as_bool() ? "true" : "false") << ")";
        return out.str();
    case lang::Value::Tag::Nil:
        return "nil";
    case lang::Value::Tag::Object: {
        const auto id = value.as_object();
        auto it = indexes.find(id);
        if (it == indexes.end()) {
            const auto index = order.size();
            indexes.emplace(id, index);
            order.push_back(id);
            it = indexes.find(id);
        }
        out << "@" << it->second;
        (void)heap.object(id);
        return out.str();
    }
    }
    return "<unknown>";
}

std::string canonical_object_graph(const lang::gc::Heap& heap, lang::ObjectId root) {
    std::map<lang::ObjectId, std::size_t> indexes;
    std::vector<lang::ObjectId> order;
    (void)value_token(heap, lang::Value::object(root), indexes, order);

    std::vector<std::pair<std::string, std::string>> fields;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& object = heap.object(order[i]);
        fields.push_back({value_token(heap, object.left, indexes, order),
                          value_token(heap, object.right, indexes, order)});
    }

    std::ostringstream out;
    out << "object(@0)";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        out << "\n  @" << i << " = pair(" << fields[i].first << ", "
            << fields[i].second << ")";
    }
    return out.str();
}

std::string observable_for(lang::VM& vm, lang::Value value) {
    vm.heap().TEST_ONLY_validate_gc_invariants();

    std::ostringstream out;
    switch (value.tag()) {
    case lang::Value::Tag::Int64:
        out << "i64:" << value.as_i64();
        break;
    case lang::Value::Tag::Bool:
        out << "bool:" << (value.as_bool() ? "true" : "false");
        break;
    case lang::Value::Tag::Nil:
        out << "nil";
        break;
    case lang::Value::Tag::Object:
        out << canonical_object_graph(vm.heap(), value.as_object());
        break;
    }
    return out.str();
}

struct Outcome {
    bool ok{false};
    std::string observable;
    std::string error;
};

Outcome execute_once(const lang::Function& function, const Schedule& schedule) {
    try {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        const auto value = vm.execute(function);
        return Outcome{true, observable_for(vm, value), {}};
    } catch (const std::exception& e) {
        return Outcome{false, {}, e.what()};
    }
}

Outcome execute_once(const lang::Module& module, const Schedule& schedule) {
    try {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        const auto value = vm.execute(module);
        return Outcome{true, observable_for(vm, value), {}};
    } catch (const std::exception& e) {
        return Outcome{false, {}, e.what()};
    }
}

const char* grammar_name(Grammar grammar) {
    switch (grammar) {
    case Grammar::Single:
        return "single";
    case Grammar::Calls:
        return "calls";
    }
    return "<unknown>";
}

Grammar parse_grammar(const std::string& value) {
    if (value == "single") {
        return Grammar::Single;
    }
    if (value == "calls") {
        return Grammar::Calls;
    }
    throw std::runtime_error("unknown grammar '" + value + "': expected single or calls");
}

std::string repro_command(Grammar grammar, std::uint64_t seed,
                          const char* schedule_name) {
    std::ostringstream out;
    out << "./build/lang_iteration5_fuzz ";
    if (grammar == Grammar::Calls) {
        out << "--grammar calls ";
    }
    out << "--seed " << seed << " --schedule " << schedule_name;
    return out.str();
}

[[noreturn]] void report_failure(std::uint64_t seed, const Schedule& schedule,
                                 const lang::Function& function,
                                 const Outcome& baseline,
                                 const Outcome& observed) {
    std::ostringstream out;
    out << "differential GC timing fuzz failure\n";
    out << "grammar=" << grammar_name(Grammar::Single) << " seed=" << seed
        << " schedule=" << schedule.name << "\n";
    out << "repro: " << repro_command(Grammar::Single, seed, schedule.name) << "\n";
    out << "program:\n" << describe(function);
    if (!baseline.ok) {
        out << "baseline trap: " << baseline.error << "\n";
    } else {
        out << "baseline observable:\n" << baseline.observable << "\n";
    }
    if (!observed.ok) {
        out << "observed trap: " << observed.error << "\n";
    } else {
        out << "observed observable:\n" << observed.observable << "\n";
    }
    throw std::runtime_error(out.str());
}

[[noreturn]] void report_failure(std::uint64_t seed, const Schedule& schedule,
                                 const lang::Module& module,
                                 const Outcome& baseline,
                                 const Outcome& observed) {
    std::ostringstream out;
    out << "differential GC timing fuzz failure\n";
    out << "grammar=" << grammar_name(Grammar::Calls) << " seed=" << seed
        << " schedule=" << schedule.name << "\n";
    out << "repro: " << repro_command(Grammar::Calls, seed, schedule.name) << "\n";
    out << "module:\n" << describe(module);
    if (!baseline.ok) {
        out << "baseline trap: " << baseline.error << "\n";
    } else {
        out << "baseline observable:\n" << baseline.observable << "\n";
    }
    if (!observed.ok) {
        out << "observed trap: " << observed.error << "\n";
    } else {
        out << "observed observable:\n" << observed.observable << "\n";
    }
    throw std::runtime_error(out.str());
}

void run_seed_schedule(std::uint64_t seed, const Schedule& schedule) {
    const auto function = generate_program(seed);
    const auto all_schedules = schedules();
    const auto& baseline_schedule = find_schedule(all_schedules, "no_stress");
    const auto baseline = execute_once(function, baseline_schedule);
    const auto observed = schedule.name == std::string("no_stress")
                              ? baseline
                              : execute_once(function, schedule);

    if (!baseline.ok || !observed.ok || baseline.observable != observed.observable) {
        report_failure(seed, schedule, function, baseline, observed);
    }
}

void run_call_seed_schedule(std::uint64_t seed, const Schedule& schedule) {
    const auto module = generate_call_module(seed);
    const auto all_schedules = schedules();
    const auto& baseline_schedule = find_schedule(all_schedules, "no_stress");
    const auto baseline = execute_once(module, baseline_schedule);
    const auto observed = schedule.name == std::string("no_stress")
                              ? baseline
                              : execute_once(module, schedule);

    if (!baseline.ok || !observed.ok || baseline.observable != observed.observable) {
        report_failure(seed, schedule, module, baseline, observed);
    }
}

void run_seed_all_schedules(std::uint64_t seed, const std::vector<Schedule>& all_schedules) {
    const auto function = generate_program(seed);
    const auto baseline = execute_once(function, all_schedules.front());
    for (const auto& schedule : all_schedules) {
        const auto observed = schedule.name == std::string(all_schedules.front().name)
                                  ? baseline
                                  : execute_once(function, schedule);
        if (!baseline.ok || !observed.ok || baseline.observable != observed.observable) {
            report_failure(seed, schedule, function, baseline, observed);
        }
    }
}

void run_call_seed_all_schedules(std::uint64_t seed,
                                 const std::vector<Schedule>& all_schedules) {
    const auto module = generate_call_module(seed);
    const auto baseline = execute_once(module, all_schedules.front());
    for (const auto& schedule : all_schedules) {
        const auto observed = schedule.name == std::string(all_schedules.front().name)
                                  ? baseline
                                  : execute_once(module, schedule);
        if (!baseline.ok || !observed.ok || baseline.observable != observed.observable) {
            report_failure(seed, schedule, module, baseline, observed);
        }
    }
}

void pinned_seed_snapshot() {
    const auto function = generate_program(kSnapshotSeed);
    const std::string expected = R"SNAPSHOT(locals=8
  #0 ConstantI64 32
  #1 ConstantI64 -32
  #2 AllocPair 0
  #3 StoreLocal 3
  #4 LoadLocal 3
  #5 LoadLocal 3
  #6 AllocPair 0
  #7 StoreLocal 2
  #8 LoadLocal 2
  #9 StoreLocal 0
  #10 Collect 0
  #11 ConstantI64 0
  #12 StoreLocal 1
  #13 LoadLocal 0
  #14 StoreLocal 5
  #15 LoadLocal 3
  #16 StoreLocal 7
  #17 ConstantI64 0
  #18 StoreLocal 6
  #19 LoadLocal 1
  #20 ConstantI64 6
  #21 LessI64 0
  #22 JumpIfFalse 53
  #23 ConstantI64 34
  #24 LoadLocal 1
  #25 AllocPair 0
  #26 StoreLocal 6
  #27 LoadLocal 3
  #28 LoadLocal 0
  #29 AllocPair 0
  #30 StoreLocal 4
  #31 LoadLocal 4
  #32 LoadLocal 4
  #33 SetLeft 0
  #34 LoadLocal 2
  #35 LoadLocal 4
  #36 SetRight 0
  #37 LoadLocal 4
  #38 StoreLocal 0
  #39 LoadLocal 0
  #40 GetLeft 0
  #41 StoreLocal 5
  #42 LoadLocal 0
  #43 GetRight 0
  #44 StoreLocal 7
  #45 ConstantI64 0
  #46 StoreLocal 6
  #47 Collect 0
  #48 LoadLocal 1
  #49 ConstantI64 1
  #50 AddI64 0
  #51 StoreLocal 1
  #52 Jump 19
  #53 LoadLocal 2
  #54 GetRight 0
  #55 Return 0
)SNAPSHOT";
    require(describe(function) == expected,
            "generator snapshot changed for seed " + std::to_string(kSnapshotSeed) +
                "\nexpected:\n" + expected + "actual:\n" + describe(function));
}

void calls_pinned_seed_snapshot() {
    const auto module = generate_call_module(kCallSnapshotSeed);
    const std::string expected = R"SNAPSHOT(entry=0 functions=5
function=0 signature=()->object locals=7
  #0 ConstantI64 6
  #1 ConstantI64 35
  #2 AllocPair 0
  #3 StoreLocal 1
  #4 LoadLocal 1
  #5 ConstantI64 22
  #6 AllocPair 0
  #7 StoreLocal 0
  #8 LoadLocal 0
  #9 StoreLocal 5
  #10 Collect 0
  #11 ConstantI64 -36
  #12 ConstantI64 38
  #13 AllocPair 0
  #14 StoreLocal 6
  #15 ConstantI64 0
  #16 StoreLocal 6
  #17 LoadLocal 1
  #18 ConstantI64 -25
  #19 ConstantI64 -34
  #20 LoadLocal 0
  #21 Call 3
  #22 StoreLocal 3
  #23 StoreLocal 5
  #24 LoadLocal 1
  #25 ConstantI64 5
  #26 LoadLocal 3
  #27 LoadLocal 0
  #28 Call 4
  #29 StoreLocal 4
  #30 StoreLocal 5
  #31 LoadLocal 0
  #32 LoadLocal 4
  #33 LoadLocal 1
  #34 LoadLocal 0
  #35 Call 1
  #36 StoreLocal 2
  #37 StoreLocal 5
  #38 LoadLocal 1
  #39 LoadLocal 0
  #40 LoadLocal 2
  #41 LoadLocal 3
  #42 LoadLocal 4
  #43 Call 2
  #44 StoreLocal 2
  #45 StoreLocal 5
  #46 LoadLocal 0
  #47 LoadLocal 2
  #48 SetLeft 0
  #49 Collect 0
  #50 LoadLocal 0
  #51 Return 0
function=1 signature=(i64,object,object)->object locals=4
  #0 LoadLocal 0
  #1 ConstantI64 1
  #2 LessI64 0
  #3 JumpIfFalse 6
  #4 LoadLocal 1
  #5 Return 0
  #6 LoadLocal 0
  #7 LoadLocal 1
  #8 AllocPair 0
  #9 StoreLocal 3
  #10 LoadLocal 3
  #11 LoadLocal 3
  #12 SetRight 0
  #13 LoadLocal 2
  #14 LoadLocal 3
  #15 SetRight 0
  #16 Collect 0
  #17 LoadLocal 0
  #18 ConstantI64 -1
  #19 AddI64 0
  #20 LoadLocal 3
  #21 LoadLocal 2
  #22 Call 1
  #23 Return 0
function=2 signature=(object,object,bool,i64)->object locals=5
  #0 LoadLocal 1
  #1 LoadLocal 3
  #2 AllocPair 0
  #3 StoreLocal 4
  #4 LoadLocal 4
  #5 LoadLocal 4
  #6 SetLeft 0
  #7 LoadLocal 2
  #8 JumpIfFalse 15
  #9 LoadLocal 0
  #10 LoadLocal 4
  #11 SetLeft 0
  #12 Collect 0
  #13 LoadLocal 4
  #14 Return 0
  #15 LoadLocal 0
  #16 LoadLocal 4
  #17 SetRight 0
  #18 Collect 0
  #19 LoadLocal 4
  #20 Return 0
function=3 signature=(i64,i64,object)->bool locals=4
  #0 LoadLocal 0
  #1 LoadLocal 2
  #2 AllocPair 0
  #3 StoreLocal 3
  #4 LoadLocal 2
  #5 LoadLocal 3
  #6 SetRight 0
  #7 Collect 0
  #8 LoadLocal 0
  #9 LoadLocal 1
  #10 LessI64 0
  #11 Return 0
function=4 signature=(i64,bool,object)->i64 locals=4
  #0 LoadLocal 0
  #1 LoadLocal 2
  #2 AllocPair 0
  #3 StoreLocal 3
  #4 LoadLocal 3
  #5 LoadLocal 3
  #6 SetRight 0
  #7 LoadLocal 2
  #8 LoadLocal 3
  #9 SetRight 0
  #10 Collect 0
  #11 LoadLocal 1
  #12 JumpIfFalse 17
  #13 LoadLocal 0
  #14 ConstantI64 1
  #15 AddI64 0
  #16 Return 0
  #17 LoadLocal 0
  #18 ConstantI64 2
  #19 AddI64 0
  #20 Return 0
)SNAPSHOT";
    require(describe(module) == expected,
            "call generator snapshot changed for seed " +
                std::to_string(kCallSnapshotSeed) + "\nexpected:\n" + expected +
                "actual:\n" + describe(module));
}

std::uint64_t parse_seed(const std::string& value) {
    std::size_t parsed = 0;
    const auto seed = std::stoull(value, &parsed, 10);
    if (parsed != value.size()) {
        throw std::runtime_error("invalid seed: " + value);
    }
    return seed;
}

int run(int argc, char** argv) {
    const auto all_schedules = schedules();

    if (argc == 5 && std::string(argv[1]) == "--seed" &&
        std::string(argv[3]) == "--schedule") {
        const auto seed = parse_seed(argv[2]);
        const auto& schedule = find_schedule(all_schedules, argv[4]);
        run_seed_schedule(seed, schedule);
        std::cerr << "[PASS] replay grammar=" << grammar_name(Grammar::Single)
                  << " seed=" << seed << " schedule=" << schedule.name << "\n";
        return 0;
    }

    if (argc == 7 && std::string(argv[1]) == "--grammar" &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--schedule") {
        const auto grammar = parse_grammar(argv[2]);
        const auto seed = parse_seed(argv[4]);
        const auto& schedule = find_schedule(all_schedules, argv[6]);
        if (grammar == Grammar::Calls) {
            run_call_seed_schedule(seed, schedule);
        } else {
            run_seed_schedule(seed, schedule);
        }
        std::cerr << "[PASS] replay grammar=" << grammar_name(grammar)
                  << " seed=" << seed << " schedule=" << schedule.name << "\n";
        return 0;
    }

    if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " [--seed <uint64> --schedule <schedule-name>]\n"
                  << "       " << argv[0]
                  << " --grammar <single|calls> --seed <uint64>"
                  << " --schedule <schedule-name>\n";
        std::cerr << "schedules:";
        for (const auto& schedule : all_schedules) {
            std::cerr << " " << schedule.name;
        }
        std::cerr << "\n";
        return 2;
    }

    pinned_seed_snapshot();
    calls_pinned_seed_snapshot();

    for (std::uint64_t seed = kFirstCorpusSeed;
         seed < kFirstCorpusSeed + kCorpusSize; ++seed) {
        run_seed_all_schedules(seed, all_schedules);
    }
    for (std::uint64_t seed = kFirstCorpusSeed;
         seed < kFirstCorpusSeed + kCorpusSize; ++seed) {
        run_call_seed_all_schedules(seed, all_schedules);
    }

    std::cerr << "[PASS] pinned_seed_snapshot seed=" << kSnapshotSeed << "\n";
    std::cerr << "[PASS] calls_pinned_seed_snapshot seed=" << kCallSnapshotSeed << "\n";
    std::cerr << "[PASS] lang_iteration5_fuzz corpus seeds=" << kCorpusSize
              << " schedules=" << all_schedules.size()
              << " executions=" << (kCorpusSize * all_schedules.size()) << "\n";
    std::cerr << "[PASS] lang_iteration5_fuzz call corpus seeds=" << kCorpusSize
              << " schedules=" << all_schedules.size()
              << " executions=" << (kCorpusSize * all_schedules.size()) << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
