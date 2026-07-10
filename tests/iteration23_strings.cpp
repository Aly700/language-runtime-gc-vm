#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr lang::ObjectId kSlotMask = 0xFFFF'FFFFull;

std::uint32_t slot_of(lang::ObjectId id) {
    return static_cast<std::uint32_t>(id & kSlotMask);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_throws(Fn&& fn, const std::string& message) {
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

template <typename Fn>
void require_throws_with_message(Fn&& fn, const std::string& expected,
                                 const std::string& message) {
    try {
        fn();
    } catch (const std::exception& e) {
        require(std::string(e.what()).find(expected) != std::string::npos,
                message + "\nexpected error containing: " + expected +
                    "\nactual error: " + e.what());
        return;
    }
    throw std::runtime_error(message);
}

struct VectorRoots final : lang::gc::RootProvider {
    std::vector<lang::Value> roots;

    void trace_roots(lang::gc::RootVisitor& visitor) override {
        for (auto& root : roots) {
            visitor.visit(root);
        }
    }
};

std::vector<std::uint8_t> object_id_bytes(lang::ObjectId id) {
    std::vector<std::uint8_t> bytes(sizeof(id));
    std::memcpy(bytes.data(), &id, sizeof(id));
    return bytes;
}

std::string diagnostics_listing(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::string rendered;
    for (const auto& diagnostic : diagnostics) {
        rendered += std::to_string(diagnostic.position.line) + ":" +
                    std::to_string(diagnostic.position.column) + " " +
                    diagnostic.message + "\n";
    }
    return rendered;
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(),
            "expected string source to compile\nsource:\n" + source +
                "\ndiagnostics:\n" + diagnostics_listing(compiled.diagnostics));
    require(compiled.verified_module.has_value(),
            "successful string source compile returned no VerifiedModule");
    return compiled;
}

void require_diagnostic(const std::string& source, std::size_t line,
                        std::size_t column, const std::string& message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected string source to be rejected\n" + source);
    require(!compiled.diagnostics.empty(),
            "rejected string source had no positioned diagnostics\n" + source);
    const auto& diagnostic = compiled.diagnostics.front();
    require(diagnostic.position.line == line &&
                diagnostic.position.column == column,
            "string diagnostic position mismatch\nexpected: " +
                std::to_string(line) + ":" + std::to_string(column) +
                "\nactual:\n" + diagnostics_listing(compiled.diagnostics));
    require(diagnostic.message.find(message) != std::string::npos,
            "string diagnostic message mismatch\nexpected substring: " + message +
                "\nactual:\n" + diagnostics_listing(compiled.diagnostics));
}

void string_payload_that_looks_like_dead_object_id_is_never_traced_or_forwarded() {
    lang::gc::Heap heap;
    const auto dead =
        heap.allocate_pair(lang::Value::int64(10), lang::Value::int64(11));
    const auto expected = object_id_bytes(dead);
    const auto string = heap.allocate_string(expected);

    static_assert(std::is_same_v<decltype(heap.string_bytes(string)),
                                 std::span<const std::uint8_t>>,
                  "string payload access must remain immutable");

    VectorRoots roots;
    roots.roots = {lang::Value::object(string)};
    heap.collect_minor(roots);

    auto moved_string = roots.roots.at(0).as_object();
    require(heap.live_count() == 1,
            "minor collection traced ObjectId-shaped string payload bytes");
    require(std::vector<std::uint8_t>(heap.string_bytes(moved_string).begin(),
                                      heap.string_bytes(moved_string).end()) == expected,
            "minor collection rewrote ObjectId-shaped string payload bytes");
    require_throws([&] { (void)heap.object(dead); },
                   "dead ObjectId survived because string bytes were treated as a reference");

    (void)heap.allocate_pair(lang::Value::int64(20), lang::Value::int64(21));
    heap.collect(roots);
    moved_string = roots.roots.at(0).as_object();

    require(heap.live_count() == 1,
            "major collection traced opaque string payload bytes");
    require(std::vector<std::uint8_t>(heap.string_bytes(moved_string).begin(),
                                      heap.string_bytes(moved_string).end()) == expected,
            "major compaction forwarded bytes inside an immutable string payload");
    heap.TEST_ONLY_validate_gc_invariants();
}

void string_storage_width_is_header_plus_rounded_payload_slots() {
    lang::gc::Heap heap;
    const std::vector<std::uint8_t> empty;
    const std::vector<std::uint8_t> eight(8, 0x41);
    const std::vector<std::uint8_t> nine(9, 0x42);

    const auto empty_string = heap.allocate_string(empty);
    const auto eight_string = heap.allocate_string(eight);
    const auto nine_string = heap.allocate_string(nine);

    require(slot_of(empty_string) == 0,
            "empty string did not reserve exactly its header slot");
    require(slot_of(eight_string) == 1,
            "eight-byte string did not start after the empty string header");
    require(slot_of(nine_string) == 3,
            "nine-byte string did not start after header plus one payload slot");
    require(heap.capacity_slots() == 6,
            "string storage width is not header plus rounded byte payload slots");
    require(heap.string_length(empty_string) == 0 &&
                heap.string_length(eight_string) == 8 &&
                heap.string_length(nine_string) == 9,
            "string descriptor byte lengths do not match payloads");
}

void mixed_heap_strings_pairs_and_arrays_compact_by_descriptor_width() {
    lang::gc::Heap heap;
    const std::vector<std::uint8_t> dead_bytes(17, 0xDD);
    const std::vector<std::uint8_t> live_bytes{
        's', 't', 'r', 'i', 'n', 'g', '-', '0', '9'};

    const auto dead_string = heap.allocate_string(dead_bytes);
    const auto scalar = heap.allocate_scalar_array(3, 7);
    const auto leaf =
        heap.allocate_pair(lang::Value::int64(5), lang::Value::int64(6));
    const auto string = heap.allocate_string(live_bytes);
    const auto refs = heap.allocate_ref_array(2, lang::Value::object(leaf));
    heap.ref_array_set(refs, 1, lang::Value::object(string));
    const auto anchor = heap.allocate_pair(lang::Value::object(refs),
                                           lang::Value::object(scalar));

    VectorRoots roots;
    roots.roots = {lang::Value::object(anchor)};
    heap.collect(roots);

    const auto moved_anchor = roots.roots.at(0).as_object();
    const auto moved_refs = heap.left(moved_anchor).as_object();
    const auto moved_scalar = heap.right(moved_anchor).as_object();
    const auto moved_leaf = heap.ref_array_get(moved_refs, 0).as_object();
    const auto moved_string = heap.ref_array_get(moved_refs, 1).as_object();

    require(slot_of(moved_scalar) == 0,
            "ScalarArray did not compact into the first dead string run");
    require(slot_of(moved_leaf) == 3,
            "Pair did not compact after the three-slot ScalarArray");
    require(slot_of(moved_string) == 4,
            "Str did not compact after Pair using its descriptor width");
    require(slot_of(moved_refs) == 7,
            "RefArray did not compact after the three-slot Str");
    require(slot_of(moved_anchor) == 9,
            "anchor Pair did not compact after the two-slot RefArray");
    require(heap.live_count() == 5,
            "mixed compaction retained a dead object or swept a live object");
    require(heap.array_get(moved_scalar, 2) == 7,
            "ScalarArray payload changed during mixed compaction");
    require(heap.left(moved_leaf).as_i64() == 5,
            "Pair payload changed during mixed compaction");
    require(std::vector<std::uint8_t>(heap.string_bytes(moved_string).begin(),
                                      heap.string_bytes(moved_string).end()) == live_bytes,
            "string bytes changed during mixed compaction");
    require_throws([&] { (void)heap.object(dead_string); },
                   "dead string ObjectId remained valid after full compaction");
    heap.TEST_ONLY_validate_gc_invariants();
}

lang::Module concat_module() {
    lang::Module module;
    module.entry_function = 0;
    module.string_constants = {"left-", "right"};

    lang::Function function;
    function.signature.return_type = lang::ValueKind::Str;
    function.code = {
        {lang::OpCode::PushStr, 0},
        {lang::OpCode::PushStr, 1},
        {lang::OpCode::StrConcat, 0},
        {lang::OpCode::Return, 0},
    };
    module.functions.push_back(std::move(function));
    return module;
}

void str_concat_operands_are_precise_roots_at_maximum_gc_stress() {
    auto report = lang::verify_module_with_diagnostics(concat_module());
    require(report.module.has_value(),
            "verifier rejected valid string concat module");
    const auto& verification = report.module->verification().functions.at(0);
    require(verification.stack_maps.at(2).object_slots ==
                std::vector<bool>({true, true}),
            "StrConcat operands were not both marked as precise object roots");
    require(verification.stack_maps.at(3).object_slots ==
                std::vector<bool>({true}),
            "StrConcat result was not marked as a precise object root");

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    stress.collect_minor_after_every_write_barrier = true;
    vm.set_gc_stress(stress);

    const auto result = vm.execute(*report.module);
    require(result.is_object(), "StrConcat returned a non-object runtime value");
    const std::string observed(vm.heap().string_bytes(result.as_object()).begin(),
                               vm.heap().string_bytes(result.as_object()).end());
    require(observed == "left-right",
            "StrConcat bytes changed while operands moved under maximum GC stress");
    vm.heap().TEST_ONLY_validate_gc_invariants();
}

void string_bytecodes_cover_empty_len_equality_and_byte_index() {
    {
        lang::Module module;
        module.string_constants = {""};
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Int64;
        function.code = {{lang::OpCode::PushStr, 0},
                         {lang::OpCode::StrLen, 0},
                         {lang::OpCode::Return, 0}};
        module.functions.push_back(std::move(function));
        lang::VM vm;
        const auto result = test_support::execute_verified(
            vm, std::move(module), "empty string StrLen module");
        require(result.as_i64() == 0, "StrLen returned nonzero for empty string");
    }

    {
        lang::Module module;
        module.string_constants = {"same", "same"};
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Bool;
        function.code = {{lang::OpCode::PushStr, 0},
                         {lang::OpCode::PushStr, 1},
                         {lang::OpCode::StrEq, 0},
                         {lang::OpCode::Return, 0}};
        module.functions.push_back(std::move(function));
        lang::VM vm;
        const auto result = test_support::execute_verified(
            vm, std::move(module), "string structural equality module");
        require(result.as_bool(), "StrEq used object identity instead of byte equality");
    }

    {
        lang::Module module;
        module.string_constants = {std::string("A\n\t\\\"")};
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Int64;
        function.code = {{lang::OpCode::PushStr, 0},
                         {lang::OpCode::ConstantI64, 1},
                         {lang::OpCode::StrIndex, 0},
                         {lang::OpCode::Return, 0}};
        module.functions.push_back(std::move(function));
        lang::VM vm;
        const auto result = test_support::execute_verified(
            vm, std::move(module), "string byte index module");
        require(result.as_i64() == 10,
                "StrIndex did not return the unsigned byte value at the requested index");
    }
}

void verifier_checks_string_pool_bounds_with_stable_reason() {
    lang::Module module;
    module.string_constants = {"only"};
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Str;
    function.code = {{lang::OpCode::PushStr, 1},
                     {lang::OpCode::Return, 0}};
    module.functions.push_back(std::move(function));

    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value(),
            "verifier accepted out-of-range string constant pool index");
    require(!report.diagnostics.empty(),
            "string pool bounds rejection had no structured diagnostic");
    require(report.diagnostics.front().reason ==
                lang::VerifierReason::BadStringConstantIndex,
            "string pool bounds rejection used the wrong stable reason code");
}

void verifier_rejects_string_operation_on_pair() {
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Int64;
    function.code = {{lang::OpCode::ConstantI64, 1},
                     {lang::OpCode::ConstantI64, 2},
                     {lang::OpCode::AllocPair, 0},
                     {lang::OpCode::StrLen, 0},
                     {lang::OpCode::Return, 0}};

    const auto report = lang::verify_with_diagnostics(function);
    require(!report.result.has_value(),
            "verifier accepted StrLen on a pair receiver");
    require(!report.diagnostics.empty(),
            "malformed string bytecode had no structured diagnostic");
    require(report.diagnostics.front().reason ==
                lang::VerifierReason::BadStringOperation,
            "malformed string bytecode used the wrong stable reason code");
}

void str_index_out_of_bounds_traps_deterministically() {
    auto make_module = [](std::int64_t index) {
        lang::Module module;
        module.string_constants = {"x"};
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Int64;
        function.code = {{lang::OpCode::PushStr, 0},
                         {lang::OpCode::ConstantI64, index},
                         {lang::OpCode::StrIndex, 0},
                         {lang::OpCode::Return, 0}};
        module.functions.push_back(std::move(function));
        return module;
    };

    lang::VM high_vm;
    require_throws_with_message(
        [&] {
            (void)test_support::execute_verified(
                high_vm, make_module(1), "high string index trap module");
        },
        "string index out of bounds",
        "StrIndex equal to byte length did not use the deterministic OOB trap");

    lang::VM negative_vm;
    require_throws_with_message(
        [&] {
            (void)test_support::execute_verified(
                negative_vm, make_module(-1), "negative string index trap module");
        },
        "string index out of bounds",
        "negative StrIndex did not use the deterministic OOB trap");
}

void frontend_literals_escapes_and_empty_string_round_trip_through_pool() {
    const std::string source = R"SRC(
let empty: str = "";
let escaped: str = "line\ncol\t\\\"";
empty + escaped
)SRC";
    const auto compiled = require_compiles(source);
    require(compiled.result_type == lang::frontend::Type::Str,
            "frontend reported the wrong public result type for str");

    const auto& module = compiled.verified_module->module();
    require(module.string_constants.size() == 2,
            "source string literals did not produce a per-module constant pool");
    require(module.string_constants.at(0).empty(),
            "empty source string changed in the module constant pool");
    require(module.string_constants.at(1) == std::string("line\ncol\t\\\""),
            "escaped source literal did not decode byte-identically into the pool");

    lang::VM vm;
    const auto result = vm.execute(*compiled.verified_module);
    const std::string observed(vm.heap().string_bytes(result.as_object()).begin(),
                               vm.heap().string_bytes(result.as_object()).end());
    require(observed == module.string_constants.at(1),
            "escaped string bytes did not round-trip from source pool through VM heap");
}

void frontend_string_operations_and_functions_preserve_compile_agreement() {
    const std::string source = R"SRC(
fn append(a: str, b: str) -> str {
  a + b
}

fn same(a: str, b: str) -> bool {
  a == b
}

let joined: str = append("ab", "cd");
let equal: bool = same(joined, "abcd");
let different: bool = joined != "abce";
let score: i64 = joined.len + joined[2];
if equal {
  score = score + 1;
} else {
  score = 0;
}
if different {
  score = score + 1;
} else {
  score = 0;
}
score
)SRC";

    const auto compiled = require_compiles(source);
    const auto& verified = *compiled.verified_module;
    const auto& module = verified.module();
    require(verified.verification().functions.size() == module.functions.size(),
            "string compile did not retain one verifier proof per function");
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        require(module.functions[i].stack_maps.size() ==
                    verified.verification().functions[i].stack_maps.size(),
                "string function stack maps did not round-trip through verifier");
        for (std::size_t pc = 0; pc < module.functions[i].stack_maps.size(); ++pc) {
            require(module.functions[i].stack_maps[pc].object_slots ==
                        verified.verification().functions[i].stack_maps[pc].object_slots,
                    "attached string stack-map bits differ from verifier proof");
        }
    }

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    vm.set_gc_stress(stress);
    const auto result = vm.execute(verified);
    require(result.as_i64() == 105,
            "frontend string concat/equality/inequality/len/index result was wrong");
}

void frontend_string_index_oob_uses_vm_trap() {
    const auto compiled = require_compiles("\"x\"[1]");
    lang::VM vm;
    require_throws_with_message(
        [&] { (void)vm.execute(*compiled.verified_module); },
        "string index out of bounds",
        "well-typed source string OOB did not reach the deterministic VM trap");
}

void frontend_rejects_new_string_type_errors_with_positions() {
    require_diagnostic("\"x\" + 1", 1, 5,
                       "operator '+' requires str + str or i64 + i64");
    require_diagnostic("\"x\" == 1", 1, 5,
                       "operator '==' requires str operands");
    require_diagnostic("\"x\"[true]", 1, 5,
                       "string index must be i64");
    require_diagnostic("1[0]", 1, 2,
                       "indexing requires array or str");
    require_diagnostic("let s: str = \"x\"; s[0] = 65; s", 1, 20,
                       "string values are immutable");
    require_diagnostic("\"bad\\q\"", 1, 5,
                       "unsupported string escape");
    require_diagnostic("\"unterminated", 1, 1,
                       "unterminated string literal");
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
        {"string_payload_that_looks_like_dead_object_id_is_never_traced_or_forwarded",
         "Str descriptor visits zero reference fields under minor and major collection",
         "RED before implementation: ObjectKind::Str and immutable byte payload APIs do not exist",
         string_payload_that_looks_like_dead_object_id_is_never_traced_or_forwarded},
        {"string_storage_width_is_header_plus_rounded_payload_slots",
         "Str storage width is one header slot plus byte payload rounded to slot granularity",
         "RED before implementation: heap has no byte-sized object descriptor",
         string_storage_width_is_header_plus_rounded_payload_slots},
        {"mixed_heap_strings_pairs_and_arrays_compact_by_descriptor_width",
         "sliding compaction advances by descriptor width across Str/Pair/ScalarArray/RefArray",
         "RED before implementation: compaction has no Str descriptor width",
         mixed_heap_strings_pairs_and_arrays_compact_by_descriptor_width},
        {"str_concat_operands_are_precise_roots_at_maximum_gc_stress",
         "Str stack slots are precise roots and concat allocation roots popped operands",
         "RED before implementation: string opcodes and verifier Str kind do not exist",
         str_concat_operands_are_precise_roots_at_maximum_gc_stress},
        {"string_bytecodes_cover_empty_len_equality_and_byte_index",
         "string opcodes preserve empty, structural, escaped-byte, and index semantics",
         "RED before implementation: PushStr/StrLen/StrEq/StrIndex do not exist",
         string_bytecodes_cover_empty_len_equality_and_byte_index},
        {"verifier_checks_string_pool_bounds_with_stable_reason",
         "PushStr pool indexes are rejected before execution with a stable reason",
         "RED before implementation: Module has no string pool or pool-bound verifier check",
         verifier_checks_string_pool_bounds_with_stable_reason},
        {"verifier_rejects_string_operation_on_pair",
         "string operations consume only the distinct verifier Str kind",
         "RED before implementation: verifier has no BadStringOperation reason",
         verifier_rejects_string_operation_on_pair},
        {"str_index_out_of_bounds_traps_deterministically",
         "StrIndex traps negative and high indexes with stable byte-oriented diagnostics",
         "RED before implementation: VM has no StrIndex trap path",
         str_index_out_of_bounds_traps_deterministically},
        {"frontend_literals_escapes_and_empty_string_round_trip_through_pool",
         "lexer-decoded literal bytes live in a per-module pool and allocate opaque strings",
         "RED before implementation: frontend has no str token, literal, or pool lowering",
         frontend_literals_escapes_and_empty_string_round_trip_through_pool},
        {"frontend_string_operations_and_functions_preserve_compile_agreement",
         "all str constructs type-check, lower, verify, round-trip maps, and execute under stress",
         "RED before implementation: frontend cannot type or lower string operations",
         frontend_string_operations_and_functions_preserve_compile_agreement},
        {"frontend_string_index_oob_uses_vm_trap",
         "well-typed source indexing preserves deterministic runtime OOB behavior",
         "RED before implementation: source string indexing does not compile",
         frontend_string_index_oob_uses_vm_trap},
        {"frontend_rejects_new_string_type_errors_with_positions",
         "mixed operations, invalid indexing, mutation, and bad escapes reject before bytecode",
         "RED before implementation: frontend has no positioned string diagnostics",
         frontend_rejects_new_string_type_errors_with_positions},
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
        std::cerr << failures << " iteration-23 string test(s) failed\n";
        return 1;
    }
    return 0;
}
