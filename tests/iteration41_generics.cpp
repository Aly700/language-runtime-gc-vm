#include "fuzz_common.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"

#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string diagnostics_listing(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.position.line << ":"
            << diagnostic.position.column << " "
            << diagnostic.message << "\n";
    }
    return out.str();
}

std::string verifier_diagnostics_listing(
    const std::vector<lang::VerifierDiagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << lang::format_verifier_diagnostic(diagnostic) << "\n";
    }
    return out.str();
}

void require_compile_boundary_agreement(
    const lang::VerifiedModule& verified, const std::string& source) {
    const auto& module = verified.module();
    const auto& original_proof = verified.verification();
    const auto reverified = lang::verify_with_diagnostics(module);
    require(
        reverified.result.has_value(),
        "type-checked generic source failed module reverification\nsource:\n" +
            source + "\nverifier diagnostics:\n" +
            verifier_diagnostics_listing(reverified.diagnostics));
    require(
        original_proof.functions.size() == module.functions.size() &&
            reverified.result->functions.size() == module.functions.size(),
        "generic compile-boundary proof omitted a concrete function");

    for (std::size_t function_index = 0;
         function_index < module.functions.size(); ++function_index) {
        const auto& function = module.functions[function_index];
        const auto& original_maps =
            original_proof.functions[function_index].stack_maps;
        const auto& repeated_maps =
            reverified.result->functions[function_index].stack_maps;
        require(
            function.stack_maps.size() == function.code.size() &&
                original_maps.size() == function.code.size() &&
                repeated_maps.size() == function.code.size(),
            "generic function " + std::to_string(function_index) +
                " omitted an instruction-boundary stack map");
        for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
            require(
                function.stack_maps[pc].object_slots ==
                        original_maps[pc].object_slots &&
                    function.stack_maps[pc].local_object_slots ==
                        original_maps[pc].local_object_slots &&
                    function.stack_maps[pc].object_slots ==
                        repeated_maps[pc].object_slots &&
                    function.stack_maps[pc].local_object_slots ==
                        repeated_maps[pc].local_object_slots,
                "generic function " + std::to_string(function_index) +
                    " stack map failed the verifier round trip at pc " +
                    std::to_string(pc));
        }
    }
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(),
            "expected generic source to compile\nsource:\n" + source +
                "\ndiagnostics:\n" +
                diagnostics_listing(compiled.diagnostics));
    require(compiled.verified_module.has_value(),
            "successful generic compile omitted VerifiedModule");
    require_compile_boundary_agreement(*compiled.verified_module, source);
    return compiled;
}

lang::frontend::Diagnostic require_diagnostic(
    const std::string& source, const std::string& expected_message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(),
            "expected generic source rejection\nsource:\n" + source);
    require(!compiled.diagnostics.empty(),
            "generic source rejection omitted diagnostics");
    const auto& diagnostic = compiled.diagnostics.front();
    require(
        diagnostic.message.find(expected_message) !=
            std::string::npos,
        "expected diagnostic containing '" + expected_message +
            "' but got '" + diagnostic.message +
            "'\nsource:\n" + source + "\nall diagnostics:\n" +
            diagnostics_listing(compiled.diagnostics));
    require(diagnostic.position.line != 0 &&
                diagnostic.position.column != 0,
            "generic diagnostic omitted source position");
    return diagnostic;
}

void require_positioned_rejection(const std::string& source) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok() && !compiled.diagnostics.empty(),
            "expected positioned generic syntax rejection\nsource:\n" +
                source);
    for (const auto& diagnostic : compiled.diagnostics) {
        require(diagnostic.position.line > 0 &&
                    diagnostic.position.column > 0,
                "generic syntax rejection omitted a source position");
    }
}

void explicit_identity_instantiates_one_concrete_function() {
    const std::string source =
        "fn id<T>(x: T) -> T { x }\n"
        "id<i64>(5)\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();

    require(module.functions.size() == 2,
            "one generic identity use must emit one concrete function");
    const auto& identity = module.functions[1];
    require(identity.signature.parameters ==
                std::vector<lang::ValueKind>{lang::ValueKind::Int64} &&
                identity.signature.return_type == lang::ValueKind::Int64,
            "id<i64> did not emit a concrete i64 signature");

    lang::VM vm;
    const auto value = vm.execute(*compiled.verified_module);
    require(value.tag() == lang::Value::Tag::Int64 &&
                value.as_i64() == 5,
            "id<i64>(5) returned the wrong value");
}

void inferred_identity_shares_one_concrete_function() {
    const std::string source =
        "fn id<T>(x: T) -> T { x }\n"
        "let first: i64 = id(4);\n"
        "id(first + 1)\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.functions.size() == 2,
            "two inferred id<i64> calls must share one instantiation");
    require(module.functions[0].code.back().op ==
                lang::OpCode::Return,
            "generic entry function did not return normally");

    lang::VM vm;
    const auto value = vm.execute(*compiled.verified_module);
    require(value.tag() == lang::Value::Tag::Int64 &&
                value.as_i64() == 5,
            "inferred shared identity returned the wrong value");
}

void multiple_parameters_substitute_through_nested_types() {
    const std::string source =
        "fn nest<T, U>(x: T, y: U) -> "
        "pair<[T], map<i64, U>> {\n"
        "  let values: [T] = array<T>(2, x);\n"
        "  let table: map<i64, U> = map<i64, U>();\n"
        "  table[0] = y;\n"
        "  pair(values, table)\n"
        "}\n"
        "nest<i64, bool>(7, true)\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.functions.size() == 2,
            "one nested generic use emitted the wrong function count");
    const auto& signature = module.functions[1].signature;
    require(signature.parameters ==
                std::vector<lang::ValueKind>{
                    lang::ValueKind::Int64,
                    lang::ValueKind::Bool} &&
                signature.return_type == lang::ValueKind::Object,
            "nested generic signature retained non-concrete types");
    require(signature.return_type_detail.has_value() &&
                signature.return_type_detail->left != nullptr &&
                signature.return_type_detail->right != nullptr &&
                signature.return_type_detail->left->kind ==
                    lang::ValueKind::Array &&
                signature.return_type_detail->right->kind ==
                    lang::ValueKind::Map,
            "nested return SignatureValue lost concrete structure");

    lang::VM vm;
    const auto value = vm.execute(*compiled.verified_module);
    require(value.tag() == lang::Value::Tag::Object,
            "nested generic function did not return its pair");
}

void distinct_instances_follow_first_use_and_share_repeats() {
    const std::string source =
        "fn id<T>(x: T) -> T { x }\n"
        "let first: bool = id<bool>(true);\n"
        "let second: i64 = id<i64>(8);\n"
        "let repeated: bool = id(first);\n"
        "second\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.functions.size() == 3,
            "bool/i64/repeated generic calls must emit two instances");
    require(module.functions[1].signature.return_type ==
                lang::ValueKind::Bool &&
                module.functions[2].signature.return_type ==
                    lang::ValueKind::Int64,
            "generic instances did not preserve first-use order");

    std::vector<std::int64_t> call_targets;
    for (const auto& instruction : module.functions[0].code) {
        if (instruction.op == lang::OpCode::Call) {
            call_targets.push_back(instruction.operand);
        }
    }
    require(call_targets ==
                std::vector<std::int64_t>{1, 2, 1},
            "generic call sites did not share canonical instances");
}

void generic_self_tail_call_closes_on_the_existing_instance() {
    const std::string source =
        "fn loop<T>(n: i64, value: T) -> T {\n"
        "  if n < 1 {\n"
        "  } else {\n"
        "    return tail loop(n + -1, value);\n"
        "  }\n"
        "  value\n"
        "}\n"
        "loop<i64>(200, 73)\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.functions.size() == 2,
            "closed generic recursion emitted duplicate instances");

    std::optional<std::size_t> tail_pc;
    for (std::size_t pc = 0;
         pc < module.functions[1].code.size(); ++pc) {
        const auto& instruction = module.functions[1].code[pc];
        if (instruction.op == lang::OpCode::TailCall) {
            tail_pc = pc;
            require(instruction.operand == 1,
                    "generic self TailCall targeted another instance");
        }
    }
    require(tail_pc.has_value(),
            "generic self recursion did not lower to TailCall");
    const auto& tail_map =
        module.functions[1].stack_maps[*tail_pc];
    require(tail_map.object_slots ==
                std::vector<bool>{false, false},
            "scalar generic TailCall exposed a reference root");

    lang::VM vm;
    vm.set_max_call_depth(2);
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Int64 &&
                result.as_i64() == 73,
            "generic tail recursion returned the wrong value");
}

void polymorphic_recursion_hits_the_stable_depth_limit() {
    const std::string source =
        "fn grow<T>(value: T) -> T {\n"
        "  let ignored: [T] = grow<[T]>([value]);\n"
        "  value\n"
        "}\n"
        "grow<i64>(1)\n";
    const auto first = require_diagnostic(
        source,
        "generic instantiation depth limit of 32 exceeded while "
        "instantiating 'grow'; possible polymorphic recursion");
    const auto second = require_diagnostic(
        source,
        "generic instantiation depth limit of 32 exceeded while "
        "instantiating 'grow'; possible polymorphic recursion");
    require(first.position.offset == second.position.offset &&
                first.message == second.message,
            "polymorphic-recursion rejection is not deterministic");
}

void generic_closure_capture_maps_are_concrete_and_movement_safe() {
    const std::string source =
        "fn make_getter<T>(value: T) -> fn() -> T {\n"
        "  fn() -> T { value }\n"
        "}\n"
        "let scalar_getter: fn() -> i64 = "
        "make_getter<i64>(4294967296);\n"
        "let object: pair<i64, i64> = pair(17, 29);\n"
        "let object_getter: fn() -> pair<i64, i64> = "
        "make_getter(object);\n"
        "let scalar: i64 = scalar_getter();\n"
        "let moved: pair<i64, i64> = object_getter();\n"
        "print(to_str(scalar));\n"
        "print(to_str(moved.left));\n"
        "pair(scalar, moved)\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.functions.size() == 5,
            "two generic getters and two concrete lambdas emitted "
            "the wrong function count");
    require(module.closure_layouts.size() == 4,
            "generic getters emitted the wrong closure layout count");
    require(module.closure_layouts[2].capture_map ==
                std::vector<bool>{false} &&
                module.closure_layouts[3].capture_map ==
                    std::vector<bool>{true},
            "scalar/reference generic captures did not produce exact "
            "concrete bitmaps");
    require(module.closure_layouts[2].capture_types[0].kind ==
                lang::ValueKind::Int64 &&
                module.closure_layouts[3].capture_types[0].kind ==
                    lang::ValueKind::Object,
            "generic closure capture signatures are not concrete");

    const auto schedules = fuzz::schedules();
    const auto baseline = fuzz::execute_once(
        *compiled.verified_module,
        fuzz::find_schedule(schedules, "no_stress"));
    require(baseline.ok && !baseline.observable.empty() &&
                !baseline.output.empty(),
            "generic closure baseline omitted a crown oracle");
    for (const auto& schedule : schedules) {
        const auto observed = fuzz::execute_once(
            *compiled.verified_module, schedule);
        require(
            observed.ok &&
                fuzz::same_observables(baseline, observed),
            "generic closure oracle drift under schedule " +
                std::string(schedule.name) + ": " +
                observed.error);
    }
}

void nested_generic_captures_keep_per_instance_bitmaps() {
    const std::string source =
        "fn nested<T>(value: T) -> fn() -> fn() -> T {\n"
        "  fn() -> fn() -> T { fn() -> T { value } }\n"
        "}\n"
        "let scalar_factory: fn() -> fn() -> i64 = "
        "nested<i64>(4294967296);\n"
        "let scalar_getter: fn() -> i64 = scalar_factory();\n"
        "let object: pair<i64, i64> = pair(23, 47);\n"
        "let object_factory: fn() -> fn() -> pair<i64, i64> = "
        "nested(object);\n"
        "let object_getter: fn() -> pair<i64, i64> = "
        "object_factory();\n"
        "print(to_str(scalar_getter()));\n"
        "let observed: pair<i64, i64> = object_getter();\n"
        "print(to_str(observed.right));\n"
        "pair(scalar_getter(), observed)\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.functions.size() == 7 &&
                module.closure_layouts.size() == 6,
            "nested generic captures emitted the wrong function/layout "
            "count");
    require(module.closure_layouts[2].capture_map ==
                std::vector<bool>{false} &&
                module.closure_layouts[3].capture_map ==
                    std::vector<bool>{false} &&
                module.closure_layouts[4].capture_map ==
                    std::vector<bool>{true} &&
                module.closure_layouts[5].capture_map ==
                    std::vector<bool>{true},
            "nested scalar/reference generic capture chain lost exact "
            "bitmaps");

    const auto schedules = fuzz::schedules();
    const auto baseline = fuzz::execute_once(
        *compiled.verified_module,
        fuzz::find_schedule(schedules, "no_stress"));
    require(baseline.ok && !baseline.observable.empty() &&
                !baseline.output.empty(),
            "nested generic capture omitted graph or output oracle");
    for (const auto& schedule : schedules) {
        const auto observed = fuzz::execute_once(
            *compiled.verified_module, schedule);
        require(observed.ok &&
                    fuzz::same_observables(baseline, observed),
                "nested generic capture drift under schedule " +
                    std::string(schedule.name) + ": " +
                    observed.error);
    }
}

bool function_has_opcode(const lang::Function& function,
                         lang::OpCode opcode) {
    for (const auto& instruction : function.code) {
        if (instruction.op == opcode) {
            return true;
        }
    }
    return false;
}

void generic_pair_array_and_record_precision_survives_movement() {
    const std::string source =
        "record ScalarBox { payload: i64 }\n"
        "record RefBox { payload: pair<i64, i64> }\n"
        "fn singleton<T>(value: T) -> [T] { [value] }\n"
        "fn duplicate<T>(value: T) -> pair<T, T> {\n"
        "  pair(value, value)\n"
        "}\n"
        "fn keep<T>(value: T) -> T { value }\n"
        "let huge: i64 = 4294967296;\n"
        "let object: pair<i64, i64> = pair(17, 29);\n"
        "let scalar_values: [i64] = singleton(huge);\n"
        "let object_values: [pair<i64, i64>] = singleton(object);\n"
        "let scalar_pair: pair<i64, i64> = duplicate(huge);\n"
        "let object_pair: pair<pair<i64, i64>, pair<i64, i64>> = "
        "duplicate(object);\n"
        "let scalar_box: ScalarBox = "
        "keep(ScalarBox { payload: huge });\n"
        "let ref_box: RefBox = "
        "keep(RefBox { payload: object });\n"
        "print(to_str(scalar_values[0]));\n"
        "print(to_str(object_values[0].right));\n"
        "if is_nil(scalar_box) {\n"
        "  print(\"nil-scalar-box\");\n"
        "} else {\n"
        "  print(to_str(scalar_box.payload));\n"
        "}\n"
        "if is_nil(ref_box) {\n"
        "  print(\"nil-ref-box\");\n"
        "} else {\n"
        "  print(to_str(ref_box.payload.left));\n"
        "}\n"
        "print(to_str(scalar_pair.right));\n"
        "print(to_str(object_pair.left.left));\n"
        "pair(scalar_values, "
        "pair(object_values, "
        "pair(scalar_box, "
        "pair(ref_box, pair(scalar_pair, object_pair)))))\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();

    require(module.functions.size() == 7,
            "array/pair/record crown emitted the wrong instance count");
    require(function_has_opcode(module.functions[1],
                                lang::OpCode::AllocArray) &&
                !function_has_opcode(module.functions[1],
                                     lang::OpCode::AllocRefArray),
            "singleton<i64> did not select the scalar array representation");
    require(function_has_opcode(module.functions[2],
                                lang::OpCode::AllocRefArray) &&
                !function_has_opcode(module.functions[2],
                                     lang::OpCode::AllocArray),
            "singleton<pair<i64,i64>> did not select the traced array "
            "representation");
    require(module.functions[3].signature.parameters ==
                std::vector<lang::ValueKind>{lang::ValueKind::Int64} &&
                module.functions[4].signature.parameters ==
                    std::vector<lang::ValueKind>{
                        lang::ValueKind::Object},
            "pair payload instantiations retained imprecise parameter facts");
    require(module.functions[5].signature.parameter_types.size() == 1 &&
                module.functions[6].signature.parameter_types.size() == 1 &&
                module.functions[5]
                        .signature.parameter_types[0]
                        .record_layout == 0 &&
                module.functions[6]
                        .signature.parameter_types[0]
                        .record_layout == 1,
            "record generic instances lost their exact nominal layouts");
    require(module.record_layouts.size() == 2 &&
                module.record_layouts[0].reference_map ==
                    std::vector<bool>{false} &&
                module.record_layouts[1].reference_map ==
                    std::vector<bool>{true},
            "scalar/reference record payload bitmaps are not exact");
    require(module.record_layouts[0].field_types.size() == 1 &&
                module.record_layouts[1].field_types.size() == 1 &&
                module.record_layouts[0].field_types[0].kind ==
                    lang::ValueKind::Int64 &&
                module.record_layouts[1].field_types[0].kind ==
                    lang::ValueKind::Object,
            "record descriptor payload types are not concrete");

    const auto schedules = fuzz::schedules();
    const auto baseline = fuzz::execute_once(
        *compiled.verified_module,
        fuzz::find_schedule(schedules, "no_stress"));
    require(baseline.ok && !baseline.observable.empty() &&
                !baseline.output.empty(),
            "array/pair/record crown omitted graph or output oracle");
    for (const auto& schedule : schedules) {
        const auto observed = fuzz::execute_once(
            *compiled.verified_module, schedule);
        require(observed.ok &&
                    fuzz::same_observables(baseline, observed),
                "array/pair/record generic precision drift under schedule " +
                    std::string(schedule.name) + ": " +
                    observed.error);
    }
}

void generic_exception_callee_reaches_a_concrete_generic_catch() {
    const std::string source =
        "variant Error { Boom(i64) }\n"
        "fn rescue<E, T>(value: T, should_throw: bool) -> T {\n"
        "  let answer: T = value;\n"
        "  try {\n"
        "    if should_throw {\n"
        "      throw Error.Boom(42);\n"
        "    } else {\n"
        "    }\n"
        "  } catch (error: E) {\n"
        "    match error {\n"
        "      Boom(code) => {\n"
        "        print(to_str(code));\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  answer\n"
        "}\n"
        "fn fail<T>(value: T) -> T {\n"
        "  throw Error.Boom(73);\n"
        "  value\n"
        "}\n"
        "let caught: i64 = 0;\n"
        "try {\n"
        "  caught = fail<i64>(1);\n"
        "} catch (error: Error) {\n"
        "  match error {\n"
        "    Boom(code) => {\n"
        "      caught = code;\n"
        "    }\n"
        "  }\n"
        "}\n"
        "let object: pair<i64, i64> = pair(11, 19);\n"
        "let preserved: pair<i64, i64> = "
        "rescue<Error, pair<i64, i64>>(object, true);\n"
        "print(to_str(caught));\n"
        "print(to_str(preserved.right));\n"
        "pair(caught, preserved)\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.functions.size() == 3,
            "generic exception paths emitted the wrong instance count");
    require(module.functions[1].exception_handlers.empty(),
            "generic throwing callee unexpectedly owns the outer handler");
    require(module.functions[2].exception_handlers.size() == 1,
            "generic catch annotation did not emit a concrete handler");
    require(module.functions[2]
                    .exception_handlers[0]
                    .variant_layout == 0,
            "generic catch handler lost its concrete variant layout");

    const auto schedules = fuzz::schedules();
    const auto baseline = fuzz::execute_once(
        *compiled.verified_module,
        fuzz::find_schedule(schedules, "no_stress"));
    require(baseline.ok && !baseline.observable.empty() &&
                !baseline.output.empty(),
            "generic exception crown omitted graph or output oracle");
    for (const auto& schedule : schedules) {
        const auto observed = fuzz::execute_once(
            *compiled.verified_module, schedule);
        require(observed.ok &&
                    fuzz::same_observables(baseline, observed),
                "generic exception oracle drift under schedule " +
                    std::string(schedule.name) + ": " +
                    observed.error);
    }
}

void nested_inference_and_all_type_positions_are_concrete() {
    const std::string source =
        "type List = pair<i64, List>;\n"
        "variant Maybe { Some(i64), None() }\n"
        "record Cell { value: i64 }\n"
        "fn array_first<T>(values: [T]) -> T { values[0] }\n"
        "fn apply<T, U>(callable: fn(T) -> U, value: T) -> U {\n"
        "  callable(value)\n"
        "}\n"
        "fn map_get<K, V>(table: map<K, V>, key: K) -> V {\n"
        "  table[key]\n"
        "}\n"
        "fn keep_weak<T>(value: weak<T>) -> weak<T> { value }\n"
        "fn keep_ephemeron<K, V>("
        "value: ephemeron<K, V>) -> ephemeron<K, V> { value }\n"
        "fn identity<T>(value: T) -> T { value }\n"
        "let values: [i64] = [31];\n"
        "let first: i64 = array_first(values);\n"
        "let plus_one: fn(i64) -> i64 = "
        "fn(value: i64) -> i64 { value + 1 };\n"
        "let applied: i64 = apply(plus_one, first);\n"
        "let table: map<i64, bool> = map<i64, bool>();\n"
        "table[4] = true;\n"
        "let found: bool = map_get(table, 4);\n"
        "let pair_value: pair<i64, i64> = pair(applied, 9);\n"
        "let observed: weak<pair<i64, i64>> = "
        "keep_weak(weak(pair_value));\n"
        "let eph: ephemeron<pair<i64, i64>, Cell> = "
        "ephemeron(pair_value, Cell { value: 88 });\n"
        "let kept: ephemeron<pair<i64, i64>, Cell> = "
        "keep_ephemeron(eph);\n"
        "let list: List = pair(5, nil);\n"
        "let same_list: List = identity(list);\n"
        "let maybe: Maybe = identity(Maybe.Some(7));\n"
        "let cell: Cell = identity(Cell { value: 12 });\n"
        "if found {\n"
        "  print(to_str(applied));\n"
        "} else {\n"
        "  print(\"bad\");\n"
        "}\n"
        "if is_nil(same_list) {\n"
        "  print(\"nil-list\");\n"
        "} else {\n"
        "  print(to_str(same_list.left));\n"
        "}\n"
        "if is_nil(cell) {\n"
        "  print(\"nil-cell\");\n"
        "} else {\n"
        "  print(to_str(cell.value));\n"
        "}\n"
        "pair(observed, pair(kept, pair(maybe, cell)))\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();
    require(module.functions.size() == 10,
            "nested inference emitted an unexpected concrete function set");
    lang::VM vm;
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Object,
            "nested inferred generic positions did not execute");
}

void inference_and_instantiation_rejections_are_stable() {
    const auto accepted = require_compiles(
        "fn increment<T>(value: T) -> T { value + 1 }\n"
        "increment<i64>(41)\n");
    lang::VM accepted_vm;
    const auto accepted_result =
        accepted_vm.execute(*accepted.verified_module);
    require(accepted_result.tag() == lang::Value::Tag::Int64 &&
                accepted_result.as_i64() == 42,
            "instantiation-site operation rejected its valid i64 case");
    require_diagnostic(
        "fn make<T>(seed: i64) -> T { seed }\n"
        "make(1)\n",
        "cannot infer type argument 'T' for generic function 'make'; "
        "use explicit type arguments");
    require_diagnostic(
        "fn same<T>(first: T, second: T) -> T { first }\n"
        "same(1, true)\n",
        "cannot infer unambiguous type arguments for generic function "
        "'same'; use explicit type arguments");
    require_diagnostic(
        "fn id<T>(value: T) -> T { value }\n"
        "id(nil)\n",
        "cannot infer type argument 'T' for generic function 'id'; "
        "use explicit type arguments");
    require_diagnostic(
        "fn id<T>(value: T) -> T { value }\n"
        "id<Missing>(1)\n",
        "unknown type 'Missing'");
    require_diagnostic(
        "fn id<T>(value: T) -> T { value }\n"
        "id<i64, bool>(1)\n",
        "generic function 'id' expects 1 type argument(s) but got 2");
    require_diagnostic(
        "fn id<T>(value: T) -> T { value }\n"
        "let callable: fn(i64) -> i64 = id;\n"
        "callable(1)\n",
        "generic function 'id' must be called with concrete type arguments");
    require_diagnostic(
        "fn increment<T>(value: T) -> T { value + 1 }\n"
        "increment<bool>(true)\n",
        "operator '+' requires i64 operands");
    require_diagnostic(
        "fn table<K, V>(value: V) -> map<K, V> {\n"
        "  let result: map<K, V> = map<K, V>();\n"
        "  result\n"
        "}\n"
        "table<pair<i64, i64>, i64>(1)\n",
        "map key type must be i64, bool, or str");
    require_diagnostic(
        "fn observe<T>(value: T) -> weak<T> { weak(value) }\n"
        "observe<i64>(1)\n",
        "weak target type must be an object type");
    require_diagnostic(
        "fn retain<K, V>(key: K, value: V) -> "
        "ephemeron<K, V> { ephemeron(key, value) }\n"
        "retain<i64, i64>(1, 2)\n",
        "ephemeron key type must be an object type");
    require_diagnostic(
        "fn wrong<T>(value: T) -> T { true }\n"
        "wrong<i64>(1)\n",
        "returns bool but is declared i64");
    require_diagnostic(
        "fn id<T, T>(value: T) -> T { value }\n"
        "id<i64, i64>(1)\n",
        "type parameter 'T' is already defined in generic function 'id'");

    require_positioned_rejection(
        "fn empty<>(value: i64) -> i64 { value }\n"
        "empty(1)\n");
    require_positioned_rejection(
        "fn malformed<T U>(value: T) -> T { value }\n"
        "malformed<i64>(1)\n");
    require_positioned_rejection(
        "fn plain(value: i64) -> i64 { value }\n"
        "plain<i64>(1)\n");
    require_positioned_rejection(
        "type Box<T> = pair<T, Box<T>>;\n"
        "let value: Box<i64, bool> = nil;\n"
        "0\n");
}

std::vector<std::int64_t> call_targets(
    const lang::Function& function) {
    std::vector<std::int64_t> result;
    for (const auto& instruction : function.code) {
        if (instruction.op == lang::OpCode::Call ||
            instruction.op == lang::OpCode::TailCall) {
            result.push_back(instruction.operand);
        }
    }
    return result;
}

void first_use_is_depth_first_and_independent_of_declaration_order() {
    const std::string source =
        "fn leaf<T>(value: T) -> T { value }\n"
        "fn root<T>(value: T) -> T { leaf<T>(value) }\n"
        "let first: i64 = root<i64>(40);\n"
        "let second: bool = leaf<bool>(true);\n"
        "let shared: i64 = root(first + 2);\n"
        "let answer: i64 = 0;\n"
        "if second { answer = shared; } else { answer = 0; }\n"
        "answer\n";
    const auto compiled = require_compiles(source);
    const auto& module = compiled.verified_module->module();

    require(module.functions.size() == 4,
            "depth-first first use emitted the wrong instance count");
    require(module.functions[1].signature.return_type ==
                lang::ValueKind::Int64 &&
                module.functions[2].signature.return_type ==
                    lang::ValueKind::Int64 &&
                module.functions[3].signature.return_type ==
                    lang::ValueKind::Bool,
            "depth-first instantiation order did not remain concrete");
    require(call_targets(module.functions[1]) ==
                std::vector<std::int64_t>{2},
            "root<i64> did not allocate leaf<i64> immediately after itself");
    require(call_targets(module.functions[0]) ==
                std::vector<std::int64_t>{1, 3, 1},
            "entry calls did not preserve first-use order and sharing");

    lang::VM vm;
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Int64 &&
                result.as_i64() == 42,
            "depth-first instance graph returned the wrong value");
}

void closed_mutual_recursion_shares_while_mutual_growth_is_rejected() {
    const std::string closed =
        "fn even<T>(n: i64, value: T) -> T {\n"
        "  if n < 1 {\n"
        "  } else {\n"
        "    return tail odd<T>(n + -1, value);\n"
        "  }\n"
        "  value\n"
        "}\n"
        "fn odd<T>(n: i64, value: T) -> T {\n"
        "  if n < 1 {\n"
        "  } else {\n"
        "    return tail even<T>(n + -1, value);\n"
        "  }\n"
        "  value\n"
        "}\n"
        "even<i64>(24, 91)\n";
    const auto compiled = require_compiles(closed);
    const auto& module = compiled.verified_module->module();
    require(module.functions.size() == 3,
            "closed mutual generic recursion duplicated an instance");
    require(call_targets(module.functions[1]) ==
                std::vector<std::int64_t>{2} &&
                call_targets(module.functions[2]) ==
                    std::vector<std::int64_t>{1},
            "closed mutual generic recursion did not close the key cycle");
    lang::VM vm;
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Int64 &&
                result.as_i64() == 91,
            "closed mutual generic recursion returned the wrong value");

    const std::string growing =
        "fn left_grow<T>(value: T) -> T {\n"
        "  let ignored: [T] = right_grow<[T]>([value]);\n"
        "  value\n"
        "}\n"
        "fn right_grow<T>(value: T) -> T {\n"
        "  let ignored: [T] = left_grow<[T]>([value]);\n"
        "  value\n"
        "}\n"
        "left_grow<i64>(1)\n";
    const auto first = require_diagnostic(
        growing,
        "generic instantiation depth limit of 32 exceeded while "
        "instantiating 'left_grow'; possible polymorphic recursion");
    const auto second = require_diagnostic(
        growing,
        "generic instantiation depth limit of 32 exceeded while "
        "instantiating 'left_grow'; possible polymorphic recursion");
    require(first.position.offset == second.position.offset &&
                first.message == second.message,
            "mutual polymorphic-recursion rejection is not deterministic");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"explicit identity instantiates one concrete function",
         explicit_identity_instantiates_one_concrete_function},
        {"inferred identity shares one concrete function",
         inferred_identity_shares_one_concrete_function},
        {"multiple parameters substitute through nested types",
         multiple_parameters_substitute_through_nested_types},
        {"distinct instances follow first use and share repeats",
         distinct_instances_follow_first_use_and_share_repeats},
        {"generic self TailCall closes on the existing instance",
         generic_self_tail_call_closes_on_the_existing_instance},
        {"polymorphic recursion hits the stable depth limit",
         polymorphic_recursion_hits_the_stable_depth_limit},
        {"generic closure capture maps are concrete and movement safe",
         generic_closure_capture_maps_are_concrete_and_movement_safe},
        {"nested generic captures keep per-instance bitmaps",
         nested_generic_captures_keep_per_instance_bitmaps},
        {"generic pair/array/record precision survives movement",
         generic_pair_array_and_record_precision_survives_movement},
        {"generic exception callee reaches a concrete generic catch",
         generic_exception_callee_reaches_a_concrete_generic_catch},
        {"nested inference and all type positions are concrete",
         nested_inference_and_all_type_positions_are_concrete},
        {"inference and instantiation rejections are stable",
         inference_and_instantiation_rejections_are_stable},
        {"first use is depth first and independent of declaration order",
         first_use_is_depth_first_and_independent_of_declaration_order},
        {"closed mutual recursion shares while mutual growth is rejected",
         closed_mutual_recursion_shares_while_mutual_growth_is_rejected},
    };

    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cerr << "[PASS] " << name << "\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] iteration41 generics: "
                  << error.what() << "\n";
        return 1;
    }
}
