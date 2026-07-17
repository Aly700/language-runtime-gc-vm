#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using lang::Value;
using lang::gc::Heap;

lang::Module module_with_entry(lang::Function entry) {
    lang::Module module;
    module.functions.push_back(std::move(entry));
    return module;
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void independent_key_activates_value() {
    Heap heap;
    auto key = heap.make_handle(heap.allocate_pair(Value::int64(1), Value::int64(2)));
    auto value = heap.allocate_pair(Value::int64(3), Value::int64(4));
    auto eph = heap.make_handle(heap.allocate_ephemeron(Value::object(key.object()),
                                                        Value::object(value), true));
    heap.collect();
    require(heap.ephemeron_key(eph.object()).as_object() == key.object(),
            "live key was not forwarded");
    require(heap.ephemeron_value(eph.object()).is_object(),
            "independently live key did not activate value");
}

void dead_key_clears_entry() {
    Heap heap;
    auto key = heap.allocate_pair(Value::int64(1), Value::int64(2));
    auto value = heap.allocate_pair(Value::int64(3), Value::int64(4));
    auto eph = heap.make_handle(heap.allocate_ephemeron(Value::object(key),
                                                        Value::object(value), true));
    heap.collect();
    require(heap.ephemeron_key(eph.object()).tag() == Value::Tag::Nil,
            "dead key was not cleared");
    require(heap.ephemeron_value(eph.object()).tag() == Value::Tag::Nil,
            "dead key retained conditional value");
    require(heap.live_count() == 1, "dead ephemeron entry retained referents");
}

void reverse_chain_requires_fixpoint() {
    Heap heap;
    auto key0 = heap.make_handle(heap.allocate_pair(Value::int64(0), Value::int64(0)));
    auto key1 = heap.allocate_pair(Value::int64(1), Value::int64(1));
    auto key2 = heap.allocate_pair(Value::int64(2), Value::int64(2));
    auto sentinel = heap.allocate_pair(Value::int64(9), Value::int64(9));
    auto e2 = heap.make_handle(heap.allocate_ephemeron(Value::object(key2),
                                                       Value::object(sentinel), true));
    auto e1 = heap.make_handle(heap.allocate_ephemeron(Value::object(key1),
                                                       Value::object(key2), true));
    auto e0 = heap.make_handle(heap.allocate_ephemeron(Value::object(key0.object()),
                                                       Value::object(key1), true));
    heap.collect();
    require(heap.ephemeron_value(e2.object()).is_object(),
            "fixpoint did not reach final conditional value");
    require(heap.metrics().ephemeron_fixpoint_passes >= 4,
            "chain did not execute productive passes plus quiescence");
    require(heap.metrics().ephemeron_activations == 3,
            "chain activation count is not exact");
}

void value_store_uses_generational_barrier() {
    Heap heap;
    auto key = heap.make_handle(heap.allocate_pair(Value::int64(0), Value::int64(0)));
    auto initial = heap.allocate_pair(Value::int64(1), Value::int64(1));
    auto eph = heap.make_handle(heap.allocate_ephemeron(Value::object(key.object()),
                                                        Value::object(initial), true));
    heap.collect_minor();
    auto replacement = heap.allocate_pair(Value::int64(7), Value::int64(8));
    heap.ephemeron_set_value(eph.object(), Value::object(replacement));
    require(heap.metrics().write_barrier_hits == 1,
            "ephemeron value store missed old-to-young barrier");
    heap.collect_minor();
    require(heap.ephemeron_value(eph.object()).is_object(),
            "barriered conditional value died in minor collection");
}

void bytecode_constructs_reads_and_mutates_ephemeron() {
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.signature.return_type_detail = lang::signature_value(lang::ValueKind::Int64);
    entry.local_count = 2;
    entry.code = {
        {lang::OpCode::ConstantI64, 7}, {lang::OpCode::ConstantI64, 8},
        {lang::OpCode::AllocPair, 0}, {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0}, {lang::OpCode::ConstantI64, 41},
        {lang::OpCode::AllocEphemeron, 0}, {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::LoadLocal, 1}, {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::EphemeronSetValue, 0},
        {lang::OpCode::LoadLocal, 1}, {lang::OpCode::EphemeronValue, 0},
        {lang::OpCode::Return, 0},
    };
    entry.signature.return_type_detail = lang::signature_value(lang::ValueKind::Int64);
    auto module = module_with_entry(entry);
    module.ephemeron_layouts.push_back(lang::EphemeronLayout{
        lang::pair_signature(lang::signature_value(lang::ValueKind::Int64),
                             lang::signature_value(lang::ValueKind::Int64)),
        lang::signature_value(lang::ValueKind::Int64), false});
    module.functions[0].code[6].operand = 0;
    auto report = lang::verify_module_with_diagnostics(std::move(module));
    require(report.module.has_value(), "verifier rejected valid ephemeron bytecode");
    lang::VM vm;
    const auto result = vm.execute(*report.module);
    require(result.as_i64() == 42, "VM ephemeron getter/setter lost scalar value");
}

void frontend_constructs_and_reads_ephemeron() {
    const auto result = lang::frontend::compile_program(R"(
let key: pair<i64, i64> = pair(7, 8);
let entry: ephemeron<pair<i64, i64>, i64> = ephemeron(key, 41);
entry.set_value(42);
entry.value()
)");
    require(result.ok(), result.diagnostics.empty()
                             ? "frontend rejected ephemeron source"
                             : result.diagnostics.front().message);
    lang::VM vm;
    const auto value = vm.execute(*result.verified_module);
    require(value.as_i64() == 42, "frontend ephemeron setter lost value");
}

void frontend_rejects_invalid_ephemerons() {
    const std::vector<std::string> sources{
        "let e: ephemeron<i64, i64> = ephemeron(1, 2); 0",
        "let p: pair<i64, i64> = pair(1, 2); let e: ephemeron<pair<i64, i64>, i64> = ephemeron(p, 2); e.set_value(true); 0",
        "let p: pair<i64, i64> = pair(1, 2); p.value()",
    };
    for (const auto& source : sources) {
        const auto result = lang::frontend::compile_program(source);
        require(!result.ok() && !result.diagnostics.empty(),
                "invalid ephemeron source was accepted");
    }
}
}

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests{
        {"independent_key_activates_value", independent_key_activates_value},
        {"dead_key_clears_entry", dead_key_clears_entry},
        {"reverse_chain_requires_fixpoint", reverse_chain_requires_fixpoint},
        {"value_store_uses_generational_barrier", value_store_uses_generational_barrier},
        {"bytecode_constructs_reads_and_mutates_ephemeron",
         bytecode_constructs_reads_and_mutates_ephemeron},
        {"frontend_constructs_and_reads_ephemeron",
         frontend_constructs_and_reads_ephemeron},
        {"frontend_rejects_invalid_ephemerons",
         frontend_rejects_invalid_ephemerons},
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
