#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"
#include "fuzz_common.hpp"

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct VectorRoots final : lang::gc::RootProvider {
    std::vector<lang::Value> values;

    void trace_roots(lang::gc::RootVisitor& visitor) override {
        for (auto& value : values) {
            visitor.visit(value);
        }
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_throws(Fn&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void require_diagnostic(const std::string& source,
                        const std::string& expected_message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(),
            "expected source to be rejected\nsource:\n" + source);
    require(!compiled.diagnostics.empty(),
            "rejected source omitted diagnostics\nsource:\n" + source);
    const auto& diagnostic = compiled.diagnostics.front();
    require(
        diagnostic.message.find(expected_message) !=
            std::string::npos,
        "diagnostic mismatch\nexpected substring: " +
            expected_message + "\nobserved: " +
            diagnostic.message + "\nsource:\n" + source);
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::ostringstream out;
        out << "expected source to compile\nsource:\n" << source << "\n";
        for (const auto& diagnostic : compiled.diagnostics) {
            out << diagnostic.position.line << ":"
                << diagnostic.position.column << " "
                << diagnostic.message << "\n";
        }
        throw std::runtime_error(out.str());
    }
    require(compiled.verified_module.has_value(),
            "successful compile omitted verified module");
    return compiled;
}

void generic_alias_record_and_variant_declarations_compile() {
    const std::string source = R"SRC(
type List<T> = pair<T, List<T>>;
record Node<T> { value: T, next: Node<T> }
variant Option<T> { None(), Some(T) }

let xs: List<i64> = pair(7, nil);
let node: Node<i64> = Node<i64> { value: 9, next: nil };
let some: Option<i64> = Option<i64>.Some(26);
42
)SRC";

    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.named_types.size() == 1,
            "List<i64> must emit one ordinary named type");
    require(module.record_layouts.size() == 1,
            "Node<i64> must emit one ordinary record layout");
    require(module.variant_layouts.size() == 1,
            "Option<i64> must emit one ordinary variant layout");

    lang::VM vm;
    require(vm.execute(*compiled.verified_module).as_i64() == 42,
            "generic nominal smoke program returned wrong result");
}

void recursive_alias_closes_on_same_key() {
    const std::string source = R"SRC(
type List<T> = pair<T, List<T>>;

fn sum(xs: List<i64>) -> i64 {
  let answer: i64 = 0;
  if is_nil(xs) {
    answer = 0;
  } else {
    answer = xs.left + sum(xs.right);
  }
  answer
}

let xs: List<i64> = nil;
let i: i64 = 0;
while i < 5 {
  xs = pair(i, xs);
  i = i + 1;
}
sum(xs)
)SRC";

    const auto compiled = require_compiles(source);
    require(compiled.verified_module->module().named_types.size() == 1,
            "same List<i64> key must close on one named type");
    lang::VM vm;
    require(vm.execute(*compiled.verified_module).as_i64() == 10,
            "recursive generic list sum returned wrong result");
}

void record_and_variant_bitmaps_are_per_instantiation() {
    const std::string source = R"SRC(
record Node<T> { value: T, next: Node<T> }
variant Option<T> { None(), Some(T) }

let scalar_node: Node<i64> = Node<i64> { value: 7, next: nil };
let object_node: Node<str> = Node<str> { value: "live", next: nil };
let scalar_option: Option<i64> = Option<i64>.Some(9);
let object_option: Option<str> = Option<str>.Some("traced");
0
)SRC";

    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.record_layouts.size() == 2,
            "Node<i64> and Node<str> need distinct layouts");
    require(module.record_layouts[0].reference_map ==
                std::vector<bool>({false, true}),
            "Node<i64> bitmap must keep scalar value opaque");
    require(module.record_layouts[1].reference_map ==
                std::vector<bool>({true, true}),
            "Node<str> bitmap must trace value and next");
    require(module.variant_layouts.size() == 2,
            "Option<i64> and Option<str> need distinct layouts");
    require(module.variant_layouts[0].cases.size() == 2 &&
                module.variant_layouts[0].cases[0].reference_map.empty() &&
                module.variant_layouts[0].cases[1].reference_map ==
                    std::vector<bool>({false}),
            "Option<i64> case maps must be exact");
    require(module.variant_layouts[1].cases.size() == 2 &&
                module.variant_layouts[1].cases[0].reference_map.empty() &&
                module.variant_layouts[1].cases[1].reference_map ==
                    std::vector<bool>({true}),
            "Option<str> case maps must be exact");
}

void generic_function_infers_through_generic_variant() {
    const std::string source = R"SRC(
variant Option<T> { None(), Some(T) }

fn or_else<T>(item: Option<T>, fallback: T) -> T {
  let answer: T = fallback;
  if is_nil(item) {
    answer = fallback;
  } else {
    match item {
      None => { answer = fallback; },
      Some(value) => { answer = value; }
    }
  }
  answer
}

let item: Option<i64> = Option<i64>.Some(42);
or_else(item, 0)
)SRC";

    const auto compiled = require_compiles(source);
    require(compiled.verified_module->module().variant_layouts.size() == 1,
            "inferred Option<i64> must share its concrete layout");
    lang::VM vm;
    require(vm.execute(*compiled.verified_module).as_i64() == 42,
            "inference through Option<T> returned wrong result");
}

void nested_generic_types_work_in_ordinary_signatures() {
    const std::string source = R"SRC(
type List<T> = pair<T, List<T>>;
variant Option<T> { None(), Some(T) }

fn keep(value: Option<List<pair<i64, str>>>)
    -> Option<List<pair<i64, str>>> {
  value
}

let value: Option<List<pair<i64, str>>> =
    Option<List<pair<i64, str>>>.None();
let kept: Option<List<pair<i64, str>>> = keep(value);
let answer: i64 = 0;
if is_nil(kept) {
  answer = 0;
} else {
  answer = 42;
}
answer
)SRC";

    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.named_types.size() == 1 &&
                module.variant_layouts.size() == 1,
            "nested concrete tuple must share one List and Option instance");
    lang::VM vm;
    require(vm.execute(*compiled.verified_module).as_i64() == 42,
            "nested generic ordinary function returned wrong result");
}

void non_closing_type_recursion_hits_depth_guard() {
    const std::string source = R"SRC(
type Grow<T> = pair<T, Grow<Grow<T>>>;
let value: Grow<i64> = nil;
0
)SRC";
    require_diagnostic(
        source,
        "generic instantiation depth limit of 32 exceeded while "
        "instantiating 'Grow'; possible polymorphic recursion");
}

void generic_declaration_rejections_are_stable() {
    require_diagnostic(R"SRC(
type Box<> = pair<i64, Box<i64>>;
0
)SRC",
                       "generic type declaration requires at least one "
                       "type parameter");
    require_diagnostic(R"SRC(
record Box<T, T> { value: T }
0
)SRC",
                       "type parameter 'T' is already defined in generic "
                       "record 'Box'");
    require_diagnostic(R"SRC(
variant Box<T, T> { Value(T) }
0
)SRC",
                       "type parameter 'T' is already defined in generic "
                       "variant 'Box'");
    require_diagnostic(R"SRC(
type Box<T> = pair<U, Box<T>>;
let value: Box<i64> = nil;
0
)SRC",
                       "unknown type 'U'");
    require_diagnostic(R"SRC(
record Box<T> { value: U }
let value: Box<i64> = Box<i64> { value: 1 };
0
)SRC",
                       "unknown type 'U'");
    require_diagnostic(R"SRC(
variant Box<T> { Value(U) }
let value: Box<i64> = Box<i64>.Value(1);
0
)SRC",
                       "unknown type 'U'");
    require_diagnostic(R"SRC(
type Box<T> = pair<T, Box<T>>;
let value: Box = nil;
0
)SRC",
                       "generic type 'Box' expects 1 type argument(s) but "
                       "got 0");
    require_diagnostic(R"SRC(
type Box<T> = pair<T, Box<T>>;
let value: Box<i64, bool> = nil;
0
)SRC",
                       "generic type 'Box' expects 1 type argument(s) but "
                       "got 2");
    require_diagnostic(R"SRC(
record Plain { value: i64 }
let value: Plain<i64> = Plain { value: 1 };
0
)SRC",
                       "type 'Plain' is not generic");
    require_diagnostic(R"SRC(
type NotRecursive<T> = T;
let value: NotRecursive<i64> = 1;
0
)SRC",
                       "must be declared as pair<...>");
}

void concrete_instantiations_remain_nominally_separate() {
    require_diagnostic(R"SRC(
record Box<T> { value: T }
let scalar: Box<i64> = Box<i64> { value: 1 };
let object: Box<str> = Box<str> { value: "one" };
scalar = object;
0
)SRC",
                       "cannot assign Box<str>");
    require_diagnostic(R"SRC(
variant Option<T> { None(), Some(T) }
let scalar: Option<i64> = Option<i64>.Some(1);
let object: Option<str> = Option<str>.Some("one");
scalar = object;
0
)SRC",
                       "cannot assign Option<str>");
    require_diagnostic(R"SRC(
record Box<T> { value: T }
let value: Box<i64> = Box<i64> { value: "wrong" };
0
)SRC",
                       "field 'value' expects i64 but got str");
    require_diagnostic(R"SRC(
variant Option<T> { None(), Some(T) }
let value: Option<i64> = Option<i64>.Some("wrong");
0
)SRC",
                       "variant payload 0 expects i64 but got str");
    require_diagnostic(R"SRC(
variant Option<T> { None(), Some(T) }
fn inspect(value: Option<i64>) -> i64 {
  let answer: i64 = 0;
  if is_nil(value) {
    answer = 0;
  } else {
    match value {
      None => { answer = 1; }
    }
  }
  answer
}
inspect(Option<i64>.Some(1))
)SRC",
                       "non-exhaustive match; missing cases: Some");
}

void compiled_generic_layouts_keep_scalar_object_ids_opaque() {
    const auto compiled = require_compiles(R"SRC(
record Node<T> { value: T, next: Node<T> }
variant Option<T> { None(), Some(T) }
let scalar_node: Node<i64> = Node<i64> { value: 1, next: nil };
let object_node: Node<pair<i64, i64>> =
    Node<pair<i64, i64>> { value: pair(2, 3), next: nil };
let scalar_option: Option<i64> = Option<i64>.Some(4);
let object_option: Option<pair<i64, i64>> =
    Option<pair<i64, i64>>.Some(pair(5, 6));
0
)SRC");
    const auto& module = compiled.verified_module->module();

    lang::gc::Heap heap;
    const auto dead = heap.allocate_pair(
        lang::Value::int64(-1), lang::Value::int64(-2));
    const auto live = heap.allocate_pair(
        lang::Value::int64(41), lang::Value::int64(42));
    const auto scalar_record = heap.allocate_record(
        0,
        {lang::Value::int64(static_cast<std::int64_t>(dead)),
         lang::Value::nil()},
        module.record_layouts[0].reference_map);
    const auto object_record = heap.allocate_record(
        1, {lang::Value::object(live), lang::Value::nil()},
        module.record_layouts[1].reference_map);
    const auto scalar_variant = heap.allocate_variant(
        0, 1,
        {lang::Value::int64(static_cast<std::int64_t>(dead))},
        {module.variant_layouts[0].cases[0].reference_map,
         module.variant_layouts[0].cases[1].reference_map});
    const auto object_variant = heap.allocate_variant(
        1, 1, {lang::Value::object(live)},
        {module.variant_layouts[1].cases[0].reference_map,
         module.variant_layouts[1].cases[1].reference_map});

    VectorRoots roots;
    roots.values = {
        lang::Value::object(scalar_record),
        lang::Value::object(object_record),
        lang::Value::object(scalar_variant),
        lang::Value::object(object_variant),
    };
    heap.set_root_provider(&roots);
    heap.collect();

    require(heap.live_count() == 5,
            "scalar generic payloads retained a dead object ID");
    require(
        heap.record_get(roots.values[0].as_object(), 0).as_i64() ==
            static_cast<std::int64_t>(dead) &&
            heap.variant_get(
                roots.values[2].as_object(), 0).as_i64() ==
                static_cast<std::int64_t>(dead),
        "scalar generic payload bits were interpreted or forwarded");
    require(
        heap.left(
                heap.record_get(
                        roots.values[1].as_object(), 0)
                    .as_object())
                .as_i64() == 41 &&
            heap.left(
                    heap.variant_get(
                            roots.values[3].as_object(), 0)
                        .as_object())
                    .as_i64() == 41,
        "object generic payloads were not retained and forwarded");
    require_throws(
        [&] { (void)heap.object(dead); },
        "dead object referenced only by scalar payload remained live");
}

void deep_scalar_and_object_graphs_survive_all_movement_schedules() {
    const std::string source = R"SRC(
record Chain<T> { value: T, next: Chain<T> }
variant Tree<T> { Leaf(T), Branch(Tree<T>, Tree<T>) }

let scalar_chain: Chain<i64> =
    Chain<i64> { value: -1, next: nil };
let object_chain: Chain<str> =
    Chain<str> { value: "initial", next: nil };
let scalar_tree: Tree<i64> = Tree<i64>.Leaf(0);
let object_tree: Tree<str> = Tree<str>.Leaf("root");
let i: i64 = 0;
while i < 18 {
  scalar_chain = Chain<i64> { value: i, next: scalar_chain };
  object_chain = Chain<str> { value: to_str(i), next: object_chain };
  scalar_tree =
      Tree<i64>.Branch(scalar_tree, Tree<i64>.Leaf(i));
  object_tree =
      Tree<str>.Branch(object_tree, Tree<str>.Leaf(to_str(i)));
  i = i + 1;
}
let replacement: Chain<str> =
    Chain<str> { value: "replacement", next: nil };
object_chain.value = "forwarded";
object_chain.next = replacement;
print("depth=" + to_str(i));
pair(pair(scalar_chain, object_chain), pair(scalar_tree, object_tree))
)SRC";

    const auto compiled = require_compiles(source);
    const auto schedules = fuzz::schedules();
    const auto baseline =
        fuzz::execute_once(*compiled.verified_module, schedules.front());
    require(baseline.ok,
            "deep generic graph baseline failed: " +
                baseline.error);
    require(!baseline.observable.empty() &&
                baseline.output == "depth=18\n",
            "deep generic graph baseline or output was vacuous");
    for (const auto& schedule : schedules) {
        const auto observed =
            fuzz::execute_once(*compiled.verified_module, schedule);
        require(
            observed.ok,
            std::string("deep generic graph failed under ") +
                schedule.name + ": " + observed.error);
        require(
            fuzz::same_observables(baseline, observed),
            std::string(
                "deep generic graph observable changed under ") +
                schedule.name);
    }
}

struct TestCase {
    const char* name;
    const char* proves;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"generic_alias_record_and_variant_declarations_compile",
         "generic aliases, records, and variants instantiate as ordinary concrete layouts",
         generic_alias_record_and_variant_declarations_compile},
        {"recursive_alias_closes_on_same_key",
         "a recursive generic alias reuses its in-progress concrete key",
         recursive_alias_closes_on_same_key},
        {"record_and_variant_bitmaps_are_per_instantiation",
         "scalar and object tuples derive distinct exact record and case bitmaps",
         record_and_variant_bitmaps_are_per_instantiation},
        {"generic_function_infers_through_generic_variant",
         "Iteration 41 inference recursively matches generic nominal provenance",
         generic_function_infers_through_generic_variant},
        {"nested_generic_types_work_in_ordinary_signatures",
         "nested applications resolve in ordinary signatures and locals",
         nested_generic_types_work_in_ordinary_signatures},
        {"non_closing_type_recursion_hits_depth_guard",
         "new-key nominal recursion is rejected by the stable shared depth guard",
         non_closing_type_recursion_hits_depth_guard},
        {"generic_declaration_rejections_are_stable",
         "malformed declarations, unbound parameters, and arity errors have stable diagnostics",
         generic_declaration_rejections_are_stable},
        {"concrete_instantiations_remain_nominally_separate",
         "record and variant instances reject cross-key values and preserve match checking",
         concrete_instantiations_remain_nominally_separate},
        {"compiled_generic_layouts_keep_scalar_object_ids_opaque",
         "concrete generic record and variant maps trace objects but ignore scalar ID bits",
         compiled_generic_layouts_keep_scalar_object_ids_opaque},
        {"deep_scalar_and_object_graphs_survive_all_movement_schedules",
         "deep scalar/object record and variant graphs survive marking and compaction schedules",
         deep_scalar_and_object_graphs_survive_all_movement_schedules},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name
                      << " | proves: " << test.proves << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name
                      << " | proves: " << test.proves << "\n"
                      << error.what() << "\n";
        }
    }
    if (failures != 0) {
        std::cerr << failures
                  << " iteration-42 generic type test(s) failed\n";
        return 1;
    }
    return 0;
}
