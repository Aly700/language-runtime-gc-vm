#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/value.hpp"
#include "lang/vm.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr lang::ObjectId kSlotMask = 0xFFFF'FFFFull;

std::uint32_t slot_of(lang::ObjectId id) {
    return static_cast<std::uint32_t>(id & kSlotMask);
}

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
    case lang::OpCode::AllocRefArray:
        return "AllocRefArray";
    case lang::OpCode::RefArrayGet:
        return "RefArrayGet";
    case lang::OpCode::RefArraySet:
        return "RefArraySet";
    case lang::OpCode::PushStr:
        return "PushStr";
    case lang::OpCode::StrLen:
        return "StrLen";
    case lang::OpCode::StrEq:
        return "StrEq";
    case lang::OpCode::StrConcat:
        return "StrConcat";
    case lang::OpCode::StrIndex:
        return "StrIndex";
    case lang::OpCode::AllocClosure:
        return "AllocClosure";
    case lang::OpCode::CallClosure:
        return "CallClosure";
    case lang::OpCode::LoadCapture:
        return "LoadCapture";
    case lang::OpCode::AllocMap:
        return "AllocMap";
    case lang::OpCode::MapSet:
        return "MapSet";
    case lang::OpCode::MapGet:
        return "MapGet";
    case lang::OpCode::MapHas:
        return "MapHas";
    case lang::OpCode::MapLen:
        return "MapLen";
    case lang::OpCode::AllocWeak:
        return "AllocWeak";
    case lang::OpCode::WeakGet:
        return "WeakGet";
    case lang::OpCode::MapKeyAt:
        return "MapKeyAt";
    case lang::OpCode::MapValueAt:
        return "MapValueAt";
    case lang::OpCode::Print:
        return "Print";
    case lang::OpCode::I64ToStr:
        return "I64ToStr";
    case lang::OpCode::StrToI64:
        return "StrToI64";
    case lang::OpCode::BoolToStr:
        return "BoolToStr";
    case lang::OpCode::StrSub:
        return "StrSub";
    case lang::OpCode::StrLt:
        return "StrLt";
    case lang::OpCode::AllocRecord:
        return "AllocRecord";
    case lang::OpCode::RecordGet:
        return "RecordGet";
    case lang::OpCode::RecordSet:
        return "RecordSet";
    case lang::OpCode::AllocVariant:
        return "AllocVariant";
    case lang::OpCode::VariantTag:
        return "VariantTag";
    case lang::OpCode::VariantGet:
        return "VariantGet";
    case lang::OpCode::TryBegin:
        return "TryBegin";
    case lang::OpCode::TryEnd:
        return "TryEnd";
    case lang::OpCode::Throw:
        return "Throw";
    default:
        break;
    }
    return "<unknown>";
}

std::string describe_function(const lang::Function& function, std::size_t index) {
    std::ostringstream out;
    out << "function=" << index << " locals=" << function.local_count << "\n";
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        const auto& ins = function.code[pc];
        out << "  #" << pc << " " << op_name(ins.op) << " " << ins.operand << "\n";
    }
    return out.str();
}

std::string describe_module(const lang::Module& module) {
    std::ostringstream out;
    out << "entry=" << module.entry_function << " functions=" << module.functions.size()
        << "\n";
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        out << describe_function(module.functions[i], i);
    }
    return out.str();
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_throws_message(Fn&& fn, const std::string& expected,
                            const std::string& message) {
    try {
        fn();
    } catch (const std::exception& e) {
        require(std::string(e.what()).find(expected) != std::string::npos,
                message + "\nwrong exception: " + e.what());
        return;
    }
    throw std::runtime_error(message + "\nno exception was thrown");
}

void set_signature(lang::Function& function,
                   std::initializer_list<lang::ValueKind> parameters,
                   lang::ValueKind result) {
    function.signature.parameters.assign(parameters.begin(), parameters.end());
    function.signature.return_type = result;
}

lang::Function add2_function() {
    lang::Function add;
    add.local_count = 2;
    set_signature(add, {lang::ValueKind::Int64, lang::ValueKind::Int64},
                  lang::ValueKind::Int64);
    add.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };
    return add;
}

void verifier_rejects_call_to_out_of_range_function() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);
    set_signature(module.functions[0], {}, lang::ValueKind::Int64);
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::Call, 7},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(module),
            "verifier accepted call to out-of-range function\n" + describe_module(module));
}

void verifier_rejects_call_with_too_few_arguments() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    set_signature(module.functions[0], {}, lang::ValueKind::Int64);
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 40},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };
    module.functions[1] = add2_function();

    require(!lang::verify(module),
            "verifier accepted call with too few arguments\n" + describe_module(module));
}

void verifier_rejects_call_with_wrong_argument_kind() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    set_signature(module.functions[0], {}, lang::ValueKind::Object);
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };
    set_signature(module.functions[1], {lang::ValueKind::Object}, lang::ValueKind::Object);
    module.functions[1].local_count = 1;
    module.functions[1].code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(module),
            "verifier accepted call with i64 where object was required\n" +
                describe_module(module));
}

void verifier_rejects_function_returning_wrong_kind() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);
    set_signature(module.functions[0], {}, lang::ValueKind::Bool);
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::Return, 0},
    };

    require(!lang::verify(module),
            "verifier accepted function returning i64 from bool signature\n" +
                describe_module(module));
}

void direct_call_executes_with_signature_checked_return() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    set_signature(module.functions[0], {}, lang::ValueKind::Int64);
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 40},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };
    module.functions[1] = add2_function();

    const auto verified = test_support::verify_module_or_throw(module,
                                                               describe_module(module));
    lang::VM vm;
    const auto result = vm.execute(verified);
    require(result.as_i64() == 42, "direct call returned wrong value\n" +
                                       describe_module(module));
}

lang::Module fib_module(std::int64_t input) {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);

    set_signature(module.functions[0], {}, lang::ValueKind::Int64);
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, input},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };

    auto& fib = module.functions[1];
    fib.local_count = 1;
    set_signature(fib, {lang::ValueKind::Int64}, lang::ValueKind::Int64);
    fib.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 6},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, -1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, -2},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };
    return module;
}

lang::Module recursive_list_module(std::int64_t depth) {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);

    auto& entry = module.functions[0];
    entry.local_count = 1;
    set_signature(entry, {}, lang::ValueKind::Object);
    entry.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, depth},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };

    auto& build = module.functions[1];
    build.local_count = 2;
    set_signature(build, {lang::ValueKind::Int64, lang::ValueKind::Object},
                  lang::ValueKind::Object);
    build.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 6},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::Return, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, -1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };
    return module;
}

struct Schedule {
    const char* name;
    lang::gc::StressConfig stress;
};

std::vector<Schedule> stress_schedules() {
    std::vector<Schedule> schedules;
    schedules.push_back({"no_stress", {}});

    lang::gc::StressConfig major_minor;
    major_minor.collect_every_n_instructions = 1;
    major_minor.collect_minor_every_n_instructions = 1;
    schedules.push_back({"major_and_minor_every_instruction", major_minor});

    lang::gc::StressConfig after_barrier;
    after_barrier.collect_before_every_allocation = true;
    after_barrier.collect_minor_after_every_write_barrier = true;
    schedules.push_back({"minor_after_every_barrier", after_barrier});

    lang::gc::StressConfig combined;
    combined.collect_before_every_allocation = true;
    combined.collect_after_every_allocation = true;
    combined.collect_every_n_instructions = 1;
    combined.collect_minor_every_n_instructions = 1;
    combined.collect_minor_after_every_write_barrier = true;
    schedules.push_back({"combined", combined});
    return schedules;
}

std::string value_token(const lang::gc::Heap& heap, lang::Value value,
                        std::map<lang::ObjectId, std::size_t>& indexes,
                        std::vector<lang::ObjectId>& order) {
    std::ostringstream out;
    switch (value.tag()) {
    case lang::Value::Tag::Int64:
        out << "i64:" << value.as_i64();
        return out.str();
    case lang::Value::Tag::Bool:
        out << "bool:" << (value.as_bool() ? "true" : "false");
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

std::string observable(lang::VM& vm, lang::Value value) {
    vm.heap().TEST_ONLY_validate_gc_invariants();
    std::map<lang::ObjectId, std::size_t> indexes;
    std::vector<lang::ObjectId> order;
    std::ostringstream out;
    out << value_token(vm.heap(), value, indexes, order);
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& object = vm.heap().object(order[i]);
        out << "\n@" << i << " = pair("
            << value_token(vm.heap(), object.left, indexes, order) << ", "
            << value_token(vm.heap(), object.right, indexes, order) << ")";
    }
    return out.str();
}

std::string execute_observable(const lang::Module& module, const Schedule& schedule) {
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    const auto result =
        test_support::execute_verified(vm, module, describe_module(module));
    return observable(vm, result);
}

std::string execute_observable(const lang::VerifiedModule& module,
                               const Schedule& schedule) {
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    const auto result = vm.execute(module);
    return observable(vm, result);
}

void recursion_matches_across_gc_stress_modes() {
    const auto schedules = stress_schedules();
    {
        const auto module = fib_module(10);
        require(lang::verify(module), "verifier rejected recursive fib module\n" +
                                          describe_module(module));
        const auto baseline = execute_observable(module, schedules.front());
        require(baseline == "i64:55", "fib baseline returned wrong value: " + baseline);
        for (const auto& schedule : schedules) {
            const auto observed = execute_observable(module, schedule);
            require(observed == baseline,
                    std::string("fib mismatch under ") + schedule.name + "\n" + observed);
        }
    }
    {
        const auto module = recursive_list_module(100);
        require(lang::verify(module), "verifier rejected recursive list module\n" +
                                          describe_module(module));
        const auto baseline = execute_observable(module, schedules.front());
        for (const auto& schedule : schedules) {
            const auto observed = execute_observable(module, schedule);
            require(observed == baseline,
                    std::string("recursive list mismatch under ") + schedule.name +
                        "\nbaseline:\n" + baseline + "\nobserved:\n" + observed);
        }
    }
}

lang::Module suspended_frame_forwarding_module() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);

    auto& entry = module.functions[0];
    entry.local_count = 3;
    set_signature(entry, {}, lang::ValueKind::Object);
    entry.code = {
        {lang::OpCode::ConstantI64, 900},
        {lang::OpCode::ConstantI64, 901},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 11},
        {lang::OpCode::ConstantI64, 12},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::ConstantI64, 6},
        {lang::OpCode::Call, 1},
        {lang::OpCode::StoreLocal, 2},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::SetLeft, 0},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::Return, 0},
    };

    auto& churn = module.functions[1];
    churn.local_count = 2;
    set_signature(churn, {lang::ValueKind::Int64}, lang::ValueKind::Int64);
    churn.code = {
        {lang::OpCode::ConstantI64, 100},
        {lang::OpCode::ConstantI64, 101},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::ConstantI64, 200},
        {lang::OpCode::ConstantI64, 201},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Return, 0},
    };
    return module;
}

void collection_in_callee_rewrites_suspended_caller_stack_and_locals() {
    const auto module = suspended_frame_forwarding_module();
    const auto verified = test_support::verify_module_or_throw(module,
                                                               describe_module(module));

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    vm.set_gc_stress(stress);
    const auto result = vm.execute(verified);

    require(result.is_object(), "suspended-frame program returned non-object");
    require(slot_of(result.as_object()) == 0,
            "test did not force movement of the caller-held object into the dead slot\n" +
                observable(vm, result));
    require(vm.heap().object(result.as_object()).left.as_object() == result.as_object(),
            "caller stack/local roots were not both valid after callee-triggered movement\n" +
                observable(vm, result));
    require(vm.heap().TEST_ONLY_validation_count() >= 1,
            "callee allocation stress did not run collection validation");
}

void call_depth_overflow_traps_deterministically() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(2);
    set_signature(module.functions[0], {}, lang::ValueKind::Int64);
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };
    auto& recurse = module.functions[1];
    recurse.local_count = 1;
    set_signature(recurse, {lang::ValueKind::Int64}, lang::ValueKind::Int64);
    recurse.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };
    const auto verified = test_support::verify_module_or_throw(module,
                                                               describe_module(module));

    lang::VM vm;
    vm.set_max_call_depth(8);
    require_throws_message([&] { (void)vm.execute(verified); }, "call depth",
                           "recursive module did not trap at configured call-depth limit");
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::ostringstream out;
        out << "expected source to compile\n" << source << "\n";
        for (const auto& diagnostic : compiled.diagnostics) {
            out << diagnostic.position.line << ":" << diagnostic.position.column << " "
                << diagnostic.message << "\n";
        }
        throw std::runtime_error(out.str());
    }
    require(compiled.verified_module.has_value(),
            "successful compile did not return a verified module");
    return compiled;
}

void require_diagnostic(const std::string& source, std::size_t line, std::size_t column,
                        const std::string& expected_message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected source to be rejected\n" + source);
    require(!compiled.diagnostics.empty(), "rejected source had no diagnostics\n" + source);
    const auto& diagnostic = compiled.diagnostics.front();
    std::ostringstream context;
    context << "diagnostic was " << diagnostic.position.line << ":"
            << diagnostic.position.column << " " << diagnostic.message << "\n"
            << source << "\n";
    require(diagnostic.position.line == line, "diagnostic line mismatch\n" + context.str());
    require(diagnostic.position.column == column,
            "diagnostic column mismatch\n" + context.str());
    require(diagnostic.message.find(expected_message) != std::string::npos,
            "diagnostic message mismatch\n" + context.str());
}

void frontend_reports_signature_misuse_with_positions() {
    require_diagnostic("fn id(a: i64) -> i64 {\n  a\n}\nid()", 4, 1,
                       "expects 1 argument");
    require_diagnostic("fn take(a: pair) -> pair {\n  a\n}\ntake(1)", 4, 6,
                       "argument 1 of function 'take' expects pair but got i64");
    require_diagnostic("fn bad() -> bool {\n  1\n}\nbad()", 2, 3,
                       "function 'bad' returns i64 but is declared bool");
    require_diagnostic("let x: i64 = 1;\nx()", 2, 1,
                       "cannot call non-function name 'x'");
}

void source_recursive_programs_are_gc_timing_equivalent() {
    const std::vector<std::string> sources = {
        R"SRC(
fn fib(n: i64) -> i64 {
  let result: i64 = 0;
  if n < 2 {
    result = n;
  } else {
    result = fib(n + -1) + fib(n + -2);
  }
  result
}
fib(8)
)SRC",
        R"SRC(
fn build(n: i64, tail: pair) -> pair {
  let result: pair = tail;
  if n < 1 {
    result = tail;
  } else {
    result = build(n + -1, pair(n, tail));
  }
  result
}
let seed: pair = pair(0, 0);
build(12, seed)
)SRC",
    };

    const auto schedules = stress_schedules();
    for (const auto& source : sources) {
        const auto compiled = require_compiles(source);
        const auto baseline =
            execute_observable(*compiled.verified_module, schedules.front());
        for (const auto& schedule : schedules) {
            const auto observed =
                execute_observable(*compiled.verified_module, schedule);
            require(observed == baseline,
                    std::string("source recursive stress mismatch under ") + schedule.name +
                        "\nsource:\n" + source + "\nbaseline:\n" + baseline +
                        "\nobserved:\n" + observed);
        }
    }
}

struct TestCase {
    const char* name;
    const char* invariant;
    const char* baseline_red;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"verifier_rejects_call_to_out_of_range_function",
         "call operands must reference a function in the module signature table",
         "RED before implementation: Module and OpCode::Call do not exist",
         verifier_rejects_call_to_out_of_range_function},
        {"verifier_rejects_call_with_too_few_arguments",
         "call sites must prove the callee signature's argument count",
         "RED before implementation: verifier has no function signatures",
         verifier_rejects_call_with_too_few_arguments},
        {"verifier_rejects_call_with_wrong_argument_kind",
         "call sites must prove argument kinds match the callee signature",
         "RED before implementation: verifier has no signature-aware Call transfer",
         verifier_rejects_call_with_wrong_argument_kind},
        {"verifier_rejects_function_returning_wrong_kind",
         "Return must prove the current function's declared return kind",
         "RED before implementation: Return accepts any non-empty stack",
         verifier_rejects_function_returning_wrong_kind},
        {"direct_call_executes_with_signature_checked_return",
         "VM dispatches Call and pushes the callee's typed return value",
         "RED before implementation: VM has no call frames",
         direct_call_executes_with_signature_checked_return},
        {"recursion_matches_across_gc_stress_modes",
         "recursive frames preserve typed values and GC roots under major/minor/barrier stress",
         "RED before implementation: VM cannot represent recursive call frames",
         recursion_matches_across_gc_stress_modes},
        {"collection_in_callee_rewrites_suspended_caller_stack_and_locals",
         "every live frame's locals and operand stack slots are mutable GC roots",
         "RED before implementation: RootProvider traces only the top-level stack/locals",
         collection_in_callee_rewrites_suspended_caller_stack_and_locals},
        {"call_depth_overflow_traps_deterministically",
         "call depth is bounded by an explicit VM limit, not by host stack exhaustion",
         "RED before implementation: recursion has no VM-level depth trap",
         call_depth_overflow_traps_deterministically},
        {"frontend_reports_signature_misuse_with_positions",
         "source calls and function returns are checked against declared signatures",
         "RED before implementation: parser/type checker has no functions or calls",
         frontend_reports_signature_misuse_with_positions},
        {"source_recursive_programs_are_gc_timing_equivalent",
         "compiled recursive source modules keep observables identical across GC schedules",
         "RED before implementation: compile_program returns only one Function",
         source_recursive_programs_are_gc_timing_equivalent},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << " | invariant: " << test.invariant
                      << " | baseline: " << test.baseline_red << "\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << " | invariant: " << test.invariant
                      << " | baseline: " << test.baseline_red << "\n"
                      << e.what() << "\n";
        }
    }

    if (failures != 0) {
        std::cerr << failures << " iteration-7 call/frame test(s) failed\n";
        return 1;
    }
    return 0;
}
