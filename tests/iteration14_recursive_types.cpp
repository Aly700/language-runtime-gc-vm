#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/value.hpp"
#include "lang/vm.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string source_listing(const std::string& source) {
    std::ostringstream out;
    out << "source:\n" << source << "\n";
    return out.str();
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

    if (!value.is_object()) {
        std::map<lang::ObjectId, std::size_t> unused_indexes;
        std::vector<lang::ObjectId> unused_order;
        return value_token(vm.heap(), value, unused_indexes, unused_order);
    }

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

struct Schedule {
    const char* name;
    lang::gc::StressConfig stress;
};

std::vector<Schedule> stress_schedules() {
    std::vector<Schedule> schedules;
    schedules.push_back({"no_stress", {}});

    lang::gc::StressConfig every_instruction;
    every_instruction.collect_every_n_instructions = 1;
    every_instruction.collect_minor_every_n_instructions = 1;
    schedules.push_back({"major_and_minor_every_instruction", every_instruction});

    lang::gc::StressConfig after_barrier;
    after_barrier.collect_before_every_allocation = true;
    after_barrier.collect_minor_after_every_write_barrier = true;
    schedules.push_back({"minor_after_every_barrier", after_barrier});
    return schedules;
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::ostringstream out;
        out << "expected source to compile\n" << source_listing(source);
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
    require(!compiled.ok(), "expected source to be rejected\n" + source_listing(source));
    require(!compiled.diagnostics.empty(),
            "rejected source did not include diagnostics\n" + source_listing(source));

    const auto& diagnostic = compiled.diagnostics.front();
    std::ostringstream context;
    context << source_listing(source);
    context << "first diagnostic: " << diagnostic.position.line << ":"
            << diagnostic.position.column << " " << diagnostic.message << "\n";

    require(diagnostic.position.line == line, "diagnostic line mismatch\n" + context.str());
    require(diagnostic.position.column == column,
            "diagnostic column mismatch\n" + context.str());
    require(diagnostic.message.find(expected_message) != std::string::npos,
            "diagnostic message mismatch\n" + context.str());
}

void require_same_observable_under_stress(const std::string& source,
                                          const std::string& expected) {
    const auto compiled = require_compiles(source);
    const auto schedules = stress_schedules();

    lang::VM baseline_vm;
    baseline_vm.set_gc_stress(schedules.front().stress);
    const auto baseline =
        observable(baseline_vm, baseline_vm.execute(*compiled.verified_module));
    require(baseline == expected,
            "baseline observable mismatch\nexpected:\n" + expected +
                "\nobserved:\n" + baseline + "\n" + source_listing(source));

    for (const auto& schedule : schedules) {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        const auto observed = schedule.name == std::string(schedules.front().name)
                                  ? baseline
                                  : observable(vm,
                                               vm.execute(*compiled.verified_module));
        require(observed == baseline,
                std::string("observable mismatch under ") + schedule.name +
                    "\nbaseline:\n" + baseline + "\nobserved:\n" + observed +
                    "\n" + source_listing(source));
    }
}

lang::SignatureValue sig_i64() {
    return lang::signature_value(lang::ValueKind::Int64);
}

lang::SignatureValue sig_bool() {
    return lang::signature_value(lang::ValueKind::Bool);
}

lang::SignatureValue sig_named(std::size_t index) {
    return lang::named_type_signature(index);
}

lang::SignatureValue sig_pair(lang::SignatureValue left, lang::SignatureValue right) {
    return lang::pair_signature(std::move(left), std::move(right));
}

void set_signature(lang::Function& function, std::vector<lang::SignatureValue> parameters,
                   lang::SignatureValue result) {
    function.signature.parameters.clear();
    function.signature.parameter_types = std::move(parameters);
    function.signature.parameters.reserve(function.signature.parameter_types.size());
    for (const auto& parameter : function.signature.parameter_types) {
        function.signature.parameters.push_back(parameter.kind);
    }
    function.signature.return_type = result.kind;
    function.signature.return_type_detail = std::move(result);
}

void recursive_list_compiles_and_sums_under_stress() {
    const std::string source = R"SRC(
type List = pair<i64, List>;

fn sum(xs: List) -> i64 {
  let total: i64 = 0;
  if is_nil(xs) {
    total = 0;
  } else {
    total = xs.left + sum(xs.right);
  }
  total
}

let xs: List = nil;
let i: i64 = 0;
while i < 5 {
  xs = pair(i, xs);
  i = i + 1;
}
sum(xs)
)SRC";

    require_same_observable_under_stress(source, "i64:10");
}

void named_recursive_tail_mutation_compiles_under_stress() {
    const std::string source = R"SRC(
type List = pair<i64, List>;

let tail: List = pair(1, nil);
let head: List = pair(0, tail);
let replacement: List = pair(41, nil);
head.right = replacement;
let read_tail: List = head.right;
let observed: i64 = 0;
if is_nil(read_tail) {
  observed = -1;
} else {
  observed = read_tail.left + 1;
}
observed
)SRC";

    require_same_observable_under_stress(source, "i64:42");
}

void mutually_recursive_named_pairs_compile_and_link_cycle() {
    const std::string source = R"SRC(
type Node = pair<i64, Links>;
type Links = pair<Node, Node>;

let first: Node = pair(10, nil);
let second: Node = pair(32, nil);
let links: Links = pair(first, second);
first.right = links;
let maybe_links: Links = first.right;
let from_links: Node = nil;
if is_nil(maybe_links) {
  from_links = nil;
} else {
  from_links = maybe_links.right;
}
let answer: i64 = 0;
if is_nil(from_links) {
  answer = -1;
} else {
  answer = from_links.left;
}
answer
)SRC";

    require_same_observable_under_stress(source, "i64:32");
}

void rejects_direct_non_pair_recursive_declaration() {
    require_diagnostic("type X = X;\nnil", 1, 6,
                       "type 'X' must be declared as pair<...>");
}

void rejects_scalar_named_type_declaration() {
    require_diagnostic("type X = i64;\nnil", 1, 6,
                       "type 'X' must be declared as pair<...>");
}

void rejects_field_read_without_nil_check() {
    const std::string source =
        "type List = pair<i64, List>;\n"
        "fn bad(xs: List) -> i64 {\n"
        "  xs.left\n"
        "}\n"
        "bad(nil)\n";
    require_diagnostic(source, 3, 6,
                       "field access requires non-nil value of type List");
}

void rejects_nested_recursive_field_read_without_second_nil_check() {
    const std::string source =
        "type List = pair<i64, List>;\n"
        "fn bad(xs: List) -> i64 {\n"
        "  let result: i64 = 0;\n"
        "  if is_nil(xs) {\n"
        "    result = 0;\n"
        "  } else {\n"
        "    result = xs.right.left;\n"
        "  }\n"
        "  result\n"
        "}\n"
        "bad(nil)\n";
    require_diagnostic(source, 7, 23,
                       "field access requires non-nil value of type List");
}

void verifier_accepts_nil_checked_recursive_signature() {
    lang::Module module;
    module.entry_function = 0;
    module.named_types.push_back(
        lang::NamedTypeSignature{"List", sig_pair(sig_i64(), sig_named(0))});
    module.functions.resize(2);

    set_signature(module.functions[0], {}, sig_i64());
    module.functions[0].code = {
        {lang::OpCode::Nil, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };

    auto& sum = module.functions[1];
    sum.local_count = 2;
    set_signature(sum, {sig_named(0)}, sig_i64());
    sum.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::IsNil, 0},
        {lang::OpCode::JumpIfFalse, 6},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::Jump, 9},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::GetLeft, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::Return, 0},
    };

    const auto verified = test_support::verify_module_or_throw(
        module, "verifier rejected nil-checked recursive signature");

    lang::VM vm;
    const auto result = vm.execute(verified);
    require(result.as_i64() == 0, "nil-checked recursive bytecode returned wrong value");
}

void verifier_rejects_mismatched_recursive_signature_without_looping() {
    lang::Module module;
    module.entry_function = 0;
    constexpr std::size_t kTypeCount = 16;
    for (std::size_t i = 0; i < kTypeCount; ++i) {
        module.named_types.push_back(lang::NamedTypeSignature{
            "Chain" + std::to_string(i),
            sig_pair(i == 0 ? sig_bool() : sig_i64(), sig_named((i + 1) % kTypeCount))});
    }
    module.functions.resize(2);

    set_signature(module.functions[0], {}, sig_i64());
    module.functions[0].code = {
        {lang::OpCode::ConstantI64, 7},
        {lang::OpCode::Nil, 0},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::Return, 0},
    };

    auto& take_chain0 = module.functions[1];
    take_chain0.local_count = 1;
    set_signature(take_chain0, {sig_named(0)}, sig_i64());
    take_chain0.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::Return, 0},
    };

    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value(),
            "verifier accepted mismatched recursive call argument");
    require(!report.diagnostics.empty(),
            "recursive signature rejection had no diagnostic");
    require(report.diagnostics.front().reason == lang::VerifierReason::BadCallArgKind,
            "recursive mismatch reported the wrong reason");
}

struct TestCase {
    const char* name;
    const char* proves;
    const char* baseline_red;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"recursive_list_compiles_and_sums_under_stress",
         "named self-recursive List can be nil-terminated, built in a loop, and summed recursively",
         "BASELINE-RED on bb2883a: lexer/parser have no type, nil, or is_nil syntax",
         recursive_list_compiles_and_sums_under_stress},
        {"named_recursive_tail_mutation_compiles_under_stress",
         "field reads and writes use the unfolded named pair shape after a nil check",
         "BASELINE-RED on bb2883a: named pair declarations do not exist",
         named_recursive_tail_mutation_compiles_under_stress},
        {"mutually_recursive_named_pairs_compile_and_link_cycle",
         "mutually recursive named pair shapes can be constructed and linked cyclically",
         "BASELINE-RED on bb2883a: named pair declarations do not exist",
         mutually_recursive_named_pairs_compile_and_link_cycle},
        {"rejects_direct_non_pair_recursive_declaration",
         "type X = X is rejected as an unfinishable non-pair recursive declaration",
         "BASELINE-RED on bb2883a: type declarations are parsed as invalid expressions",
         rejects_direct_non_pair_recursive_declaration},
        {"rejects_scalar_named_type_declaration",
         "named declarations must unfold to a pair shape, not a scalar",
         "BASELINE-RED on bb2883a: type declarations are parsed as invalid expressions",
         rejects_scalar_named_type_declaration},
        {"rejects_field_read_without_nil_check",
         "nullable named pair parameters cannot be dereferenced without is_nil refinement",
         "BASELINE-RED on bb2883a: named pair declarations do not exist",
         rejects_field_read_without_nil_check},
        {"rejects_nested_recursive_field_read_without_second_nil_check",
         "recursive fields read as nullable named pairs and require their own nil check",
         "BASELINE-RED on bb2883a: named pair declarations do not exist",
         rejects_nested_recursive_field_read_without_second_nil_check},
        {"verifier_accepts_nil_checked_recursive_signature",
         "bytecode verifier refines is_nil branches and accepts recursive named signatures",
         "BASELINE-RED on bb2883a: bytecode has no named type table, Nil, or IsNil",
         verifier_accepts_nil_checked_recursive_signature},
        {"verifier_rejects_mismatched_recursive_signature_without_looping",
         "recursive signature conformance terminates and rejects a deep mismatched shape",
         "BASELINE-RED on bb2883a: recursive signature references are unrepresentable",
         verifier_rejects_mismatched_recursive_signature_without_looping},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << " | proves: " << test.proves
                      << " | baseline: " << test.baseline_red << "\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << " | proves: " << test.proves
                      << " | baseline: " << test.baseline_red << "\n"
                      << e.what() << "\n";
        }
    }

    if (failures != 0) {
        std::cerr << failures << " iteration-14 recursive type test(s) failed\n";
        return 1;
    }
    return 0;
}
