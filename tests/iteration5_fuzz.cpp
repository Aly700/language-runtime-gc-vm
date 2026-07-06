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
constexpr std::uint64_t kFirstCorpusSeed = 1;
constexpr std::uint64_t kCorpusSize = 64;

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

class Builder {
public:
    explicit Builder(std::uint32_t local_count) {
        function_.local_count = local_count;
        function_.signature.return_type = lang::ValueKind::Object;
        locals_.resize(local_count);
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
        pop_any();
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

std::string repro_command(std::uint64_t seed, const char* schedule_name) {
    std::ostringstream out;
    out << "./build/lang_iteration5_fuzz --seed " << seed << " --schedule "
        << schedule_name;
    return out.str();
}

[[noreturn]] void report_failure(std::uint64_t seed, const Schedule& schedule,
                                 const lang::Function& function,
                                 const Outcome& baseline,
                                 const Outcome& observed) {
    std::ostringstream out;
    out << "differential GC timing fuzz failure\n";
    out << "seed=" << seed << " schedule=" << schedule.name << "\n";
    out << "repro: " << repro_command(seed, schedule.name) << "\n";
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
        std::cerr << "[PASS] replay seed=" << seed << " schedule=" << schedule.name
                  << "\n";
        return 0;
    }

    if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " [--seed <uint64> --schedule <schedule-name>]\n";
        std::cerr << "schedules:";
        for (const auto& schedule : all_schedules) {
            std::cerr << " " << schedule.name;
        }
        std::cerr << "\n";
        return 2;
    }

    pinned_seed_snapshot();

    for (std::uint64_t seed = kFirstCorpusSeed;
         seed < kFirstCorpusSeed + kCorpusSize; ++seed) {
        run_seed_all_schedules(seed, all_schedules);
    }

    std::cerr << "[PASS] pinned_seed_snapshot seed=" << kSnapshotSeed << "\n";
    std::cerr << "[PASS] lang_iteration5_fuzz corpus seeds=" << kCorpusSize
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
