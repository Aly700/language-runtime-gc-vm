#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"
#include "fuzz_common.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string string_value(const lang::VM& vm, lang::Value value) {
    require(value.is_object(), "expected string object value");
    const auto bytes = vm.heap().string_bytes(value.as_object());
    return std::string(bytes.begin(), bytes.end());
}

std::string diagnostics_listing(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.position.line << ":" << diagnostic.position.column
            << " " << diagnostic.message << "\n";
    }
    return out.str();
}

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(), "expected source to compile\n" + source + "\n" +
                               diagnostics_listing(compiled.diagnostics));
    require(compiled.verified_module.has_value(),
            "successful compilation returned no verified module");
    return compiled;
}

void require_diagnostic(const std::string& source, std::size_t line,
                        std::size_t column, const std::string& message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected source rejection\n" + source);
    for (const auto& diagnostic : compiled.diagnostics) {
        if (diagnostic.position.line == line &&
            diagnostic.position.column == column &&
            diagnostic.message.find(message) != std::string::npos) {
            return;
        }
    }
    throw std::runtime_error("missing diagnostic '" + message + "' at " +
                             std::to_string(line) + ":" +
                             std::to_string(column) + "\n" +
                             diagnostics_listing(compiled.diagnostics));
}

lang::VerifiedModule verify(lang::Module module, const std::string& context) {
    return test_support::verify_module_or_throw(std::move(module), context);
}

lang::Module substring_module(std::string source, std::int64_t lo,
                              std::int64_t hi, bool return_pair = false) {
    lang::Module module;
    module.string_constants.push_back(std::move(source));
    lang::Function function;
    function.local_count = return_pair ? 2 : 0;
    function.signature.return_type =
        return_pair ? lang::ValueKind::Object : lang::ValueKind::Str;
    function.code = {{lang::OpCode::PushStr, 0}};
    if (return_pair) {
        function.code.push_back({lang::OpCode::StoreLocal, 0});
        function.code.push_back({lang::OpCode::LoadLocal, 0});
    }
    function.code.push_back({lang::OpCode::ConstantI64, lo});
    function.code.push_back({lang::OpCode::ConstantI64, hi});
    function.code.push_back({lang::OpCode::StrSub, 0});
    if (return_pair) {
        function.code.push_back({lang::OpCode::StoreLocal, 1});
        function.code.push_back({lang::OpCode::LoadLocal, 0});
        function.code.push_back({lang::OpCode::LoadLocal, 1});
        function.code.push_back({lang::OpCode::AllocPair, 0});
    }
    function.code.push_back({lang::OpCode::Return, 0});
    module.functions.push_back(std::move(function));
    return module;
}

lang::Module ordering_module(std::string left, std::string right) {
    lang::Module module;
    module.string_constants = {std::move(left), std::move(right)};
    lang::Function function;
    function.signature.return_type = lang::ValueKind::Bool;
    function.code = {{lang::OpCode::PushStr, 0},
                     {lang::OpCode::PushStr, 1},
                     {lang::OpCode::StrLt, 0},
                     {lang::OpCode::Return, 0}};
    module.functions.push_back(std::move(function));
    return module;
}

void verifier_types_new_opcodes_and_marks_sub_source_as_a_root() {
    const auto verified = verify(substring_module("abcdef", 1, 4),
                                 "valid StrSub module");
    const auto& maps = verified.verification().functions.at(0).stack_maps;
    require(maps.at(3).object_slots ==
                std::vector<bool>({true, false, false}),
            "StrSub stack map did not precisely root only the source string");
    require(maps.at(4).object_slots == std::vector<bool>({true}),
            "StrSub result was not a precise string root");

    {
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Str;
        function.code = {{lang::OpCode::ConstantI64, 0},
                         {lang::OpCode::ConstantI64, 0},
                         {lang::OpCode::ConstantI64, 0},
                         {lang::OpCode::StrSub, 0},
                         {lang::OpCode::Return, 0}};
        const auto report = lang::verify_with_diagnostics(function);
        require(!report.result.has_value() && !report.diagnostics.empty() &&
                    report.diagnostics.front().reason ==
                        lang::VerifierReason::StrSubRequiresStr,
                "StrSub receiver confusion lacked its stable reason");
    }
    {
        lang::Module module;
        module.string_constants = {"x"};
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Str;
        function.code = {{lang::OpCode::PushStr, 0},
                         {lang::OpCode::PushStr, 0},
                         {lang::OpCode::ConstantI64, 1},
                         {lang::OpCode::StrSub, 0},
                         {lang::OpCode::Return, 0}};
        module.functions.push_back(std::move(function));
        const auto report = lang::verify_with_diagnostics(module);
        require(!report.result.has_value() && !report.diagnostics.empty() &&
                    report.diagnostics.front().reason ==
                        lang::VerifierReason::StrSubRequiresI64Bounds,
                "StrSub bound confusion lacked its stable reason");
    }
    {
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Bool;
        function.code = {{lang::OpCode::ConstantI64, 0},
                         {lang::OpCode::ConstantI64, 1},
                         {lang::OpCode::StrLt, 0},
                         {lang::OpCode::Return, 0}};
        const auto report = lang::verify_with_diagnostics(function);
        require(!report.result.has_value() && !report.diagnostics.empty() &&
                    report.diagnostics.front().reason ==
                        lang::VerifierReason::StrLtRequiresStr,
                "StrLt operand confusion lacked its stable reason");
    }
}

void substring_trap_matrix_and_copy_identity_are_deterministic() {
    for (const auto& bounds :
         std::vector<std::pair<std::int64_t, std::int64_t>>{
             {-1, 2}, {0, 7}, {4, 3}}) {
        const auto verified = verify(
            substring_module("abcdef", bounds.first, bounds.second),
            "trapping StrSub module");
        std::string baseline;
        for (const auto& schedule : fuzz::schedules()) {
            lang::VM vm;
            vm.set_gc_stress(schedule.stress);
            try {
                (void)vm.execute(verified);
                throw std::runtime_error("StrSub invalid bounds did not trap");
            } catch (const std::out_of_range& error) {
                const std::string observed = error.what();
                require(observed.find("string substring bounds out of range") !=
                            std::string::npos,
                        "StrSub used the wrong stable diagnostic");
                if (baseline.empty()) {
                    baseline = observed;
                } else {
                    require(observed == baseline,
                            "StrSub trap diagnostic drifted across schedules");
                }
            }
        }
    }

    for (const auto& [lo, hi, expected] :
         std::vector<std::tuple<std::int64_t, std::int64_t, std::string>>{
             {2, 2, ""}, {0, 6, "abcdef"}, {1, 4, "bcd"}}) {
        const auto verified = verify(substring_module("abcdef", lo, hi),
                                     "valid StrSub boundary module");
        for (const auto& schedule : fuzz::schedules()) {
            lang::VM vm;
            vm.set_gc_stress(schedule.stress);
            require(string_value(vm, vm.execute(verified)) == expected,
                    "StrSub returned incorrect half-open bytes");
        }
    }

    lang::VM vm;
    const auto pair = vm.execute(verify(substring_module("abcdef", 0, 6, true),
                                        "full-range copy module"));
    const auto source = vm.heap().left(pair.as_object());
    const auto copy = vm.heap().right(pair.as_object());
    require(source.as_object() != copy.as_object(),
            "full-range StrSub shared identity instead of copying");
    require(string_value(vm, source) == string_value(vm, copy),
            "full-range StrSub copy changed bytes");
}

void str_lt_is_unsigned_bytewise_lexicographic() {
    const std::string zero(1, '\0');
    const std::string ff(1, static_cast<char>(0xff));
    const std::vector<std::tuple<std::string, std::string, bool>> cases = {
        {"", "a", true},
        {"a", "", false},
        {"ab", "abc", true},
        {"abc", "ab", false},
        {"a", "b", true},
        {"ab", "ac", true},
        {"ab", "ab", false},
        {zero, ff, true},
        {ff, zero, false},
    };
    for (const auto& [left, right, expected] : cases) {
        const auto verified = verify(ordering_module(left, right),
                                     "valid StrLt module");
        for (const auto& schedule : fuzz::schedules()) {
            lang::VM vm;
            vm.set_gc_stress(schedule.stress);
            require(vm.execute(verified).as_bool() == expected,
                    "StrLt violated unsigned byte-wise lexicographic order");
        }
    }
}

void substring_source_survives_movement_and_allocating_copy() {
    lang::Module module;
    module.string_constants = {"prefix-source-suffix", "garbage"};
    lang::Function function;
    function.local_count = 3;
    function.signature.return_type = lang::ValueKind::Object;
    function.code = {
        {lang::OpCode::PushStr, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::ConstantI64, 24},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 17},
        {lang::OpCode::PushStr, 1},
        {lang::OpCode::PushStr, 1},
        {lang::OpCode::StrConcat, 0},
        {lang::OpCode::StoreLocal, 2},
        {lang::OpCode::LoadLocal, 1},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::Jump, 4},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 7},
        {lang::OpCode::ConstantI64, 13},
        {lang::OpCode::StrSub, 0},
        {lang::OpCode::StoreLocal, 2},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::LoadLocal, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Return, 0},
    };
    module.functions.push_back(std::move(function));
    const auto verified = verify(std::move(module), "StrSub rooting crown jewel");

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    stress.collect_minor_after_every_write_barrier = true;
    vm.set_gc_stress(stress);
    const auto result = vm.execute(verified);
    const auto source = vm.heap().left(result.as_object());
    const auto substring = vm.heap().right(result.as_object());
    require(string_value(vm, source) == "prefix-source-suffix",
            "StrSub allocation corrupted or lost its moved source root");
    require(string_value(vm, substring) == "source",
            "StrSub bytes changed while source and result moved");
    vm.heap().TEST_ONLY_validate_gc_invariants();
}

void frontend_derives_all_string_ordering_operators() {
    const std::vector<std::pair<std::string, bool>> cases = {
        {R"("" < "a")", true},
        {R"("ab" < "abc")", true},
        {R"("b" < "a")", false},
        {R"("ab" <= "ab")", true},
        {R"("abc" <= "ab")", false},
        {R"("b" > "a")", true},
        {R"("a" > "b")", false},
        {R"("ab" >= "ab")", true},
        {R"("ab" >= "abc")", false},
        {R"("same" < "same")", false},
        {R"("same" > "same")", false},
        {R"("same" <= "same")", true},
        {R"("same" >= "same")", true},
    };
    for (const auto& [source, expected] : cases) {
        const auto compiled = require_compiles(source + "\n");
        const auto raw_report =
            lang::verify_with_diagnostics(compiled.verified_module->module());
        require(raw_report.result.has_value(),
                "frontend string ordering violated compiler/verifier agreement");
        for (const auto& schedule : fuzz::schedules()) {
            lang::VM vm;
            vm.set_gc_stress(schedule.stress);
            require(vm.execute(*compiled.verified_module).as_bool() == expected,
                    "frontend string ordering returned the wrong bool for " + source);
        }
    }
}

void reversed_string_ordering_preserves_left_to_right_operand_evaluation() {
    const auto compiled = require_compiles(R"SRC(
fn mark(table: map<i64, i64>, text: str, value: i64) -> str {
  table[0] = value;
  text
}

let table: map<i64, i64> = map<i64, i64>();
let ordered: bool = mark(table, "a", 1) > mark(table, "b", 2);
table[0]
)SRC");
    for (const auto& schedule : fuzz::schedules()) {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        require(vm.execute(*compiled.verified_module).as_i64() == 2,
                "string ordering reversed left-to-right operand evaluation");
    }
}

void frontend_sub_composes_with_maps_output_closures_and_for_in_break() {
    const std::string source = R"SRC(
fn hold(value: str) -> fn() -> str {
  fn() -> str { value }
}

let original: str = "xx-key-yy";
let key: str = original.sub(3, 6);
let captured: fn() -> str = hold(key);
let table: map<str, str> = map<str, str>();
table[key] = "mapped";
let seen: bool = false;
print(key);
for candidate, value in table {
  if candidate < "zzz" {
    seen = true;
    break;
  } else {
    seen = false;
  }
}
pair(captured(), table["key"] + to_str(seen))
)SRC";
    const auto compiled = require_compiles(source);
    for (const auto& schedule : fuzz::schedules()) {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        const auto result = vm.execute(*compiled.verified_module);
        require(string_value(vm, vm.heap().left(result.as_object())) == "key",
                "closure capture changed substring bytes");
        require(string_value(vm, vm.heap().right(result.as_object())) ==
                    "mappedtrue",
                "substring map key was not structurally equal to literal key");
        require(std::string(vm.output().begin(), vm.output().end()) == "key\n",
                "substring output bytes drifted");
    }
}

void frontend_rejects_mixed_ordering_and_bad_sub_calls_with_positions() {
    require_diagnostic("\"x\" < 1\n", 1, 5,
                       "operator '<' requires matching i64 or str operands");
    require_diagnostic("1 >= \"x\"\n", 1, 3,
                       "operator '>=' requires str operands");
    require_diagnostic("\"abc\".sub(0)\n", 1, 7,
                       "sub expects exactly 2 arguments");
    require_diagnostic("\"abc\".sub(0, 1, 2)\n", 1, 7,
                       "sub expects exactly 2 arguments");
    require_diagnostic("\"abc\".sub(true, 2)\n", 1, 11,
                       "sub argument expects i64");
    require_diagnostic("\"abc\".sub(0, false)\n", 1, 14,
                       "sub argument expects i64");
    require_diagnostic("1.sub(0, 1)\n", 1, 3, "sub requires str receiver");
}

constexpr std::uint64_t kStrings2SnapshotSeed = 31;
constexpr std::uint64_t kStrings2FirstSeed = 1;
constexpr std::uint64_t kStrings2CorpusSize = 32;
constexpr std::uint64_t kStrings2MutantCorpusSize = 10;

std::string generate_strings2_source(std::uint64_t seed) {
    const auto token = std::string("s") + std::to_string((seed * 37) % 997);
    const auto prefix = seed % 2 == 0 ? "zz-" : "aa-";
    const auto raw = std::string(prefix) + token + "-tail";
    const auto hi = 3 + token.size();
    const auto comparison =
        seed % 4 == 0 ? "piece < \"zzzz\"" :
        seed % 4 == 1 ? "piece <= piece" :
        seed % 4 == 2 ? "piece > \"\"" : "piece >= piece";

    std::ostringstream out;
    out << "fn retain(value: str) -> fn() -> str {\n"
        << "  fn() -> str { value }\n"
        << "}\n\n"
        << "let raw: str = \"" << raw << "\";\n"
        << "let lo: i64 = 3;\n"
        << "let hi: i64 = " << hi << ";\n"
        << "let piece: str = raw.sub(lo, hi);\n"
        << "let joined: str = piece + \"-x\";\n"
        << "let ordered: bool = " << comparison << ";\n"
        << "let length: i64 = joined.len;\n"
        << "let first: i64 = joined[0];\n"
        << "let table: map<str, str> = map<str, str>();\n"
        << "table[piece] = joined;\n"
        << "let render: fn() -> str = retain(piece);\n"
        << "print(piece);\n"
        << "print(joined.sub(0, piece.len));\n"
        << "print(to_str(length));\n"
        << "print(to_str(first));\n"
        << "print(to_str(ordered));\n"
        << "for key, value in table {\n"
        << "  if key >= piece {\n"
        << "    print(value.sub(piece.len, value.len));\n"
        << "    break;\n"
        << "  } else {\n"
        << "    print(\"unreachable\");\n"
        << "  }\n"
        << "}\n"
        << "pair(render(), table[\"" << token << "\"])\n";
    return out.str();
}

lang::VerifiedModule compile_strings2_source(std::uint64_t seed) {
    const auto source = generate_strings2_source(seed);
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(), "strings2 grammar rejected seed=" +
                               std::to_string(seed) + "\n" + source + "\n" +
                               diagnostics_listing(compiled.diagnostics));
    const auto report =
        lang::verify_with_diagnostics(compiled.verified_module->module());
    require(report.result.has_value(),
            "strings2 grammar violated compiler/verifier agreement seed=" +
                std::to_string(seed));
    return *compiled.verified_module;
}

void run_strings2_seed_schedule(std::uint64_t seed,
                                const fuzz::Schedule& schedule) {
    const auto verified = compile_strings2_source(seed);
    const auto all_schedules = fuzz::schedules();
    const auto baseline = fuzz::execute_once(
        verified, fuzz::find_schedule(all_schedules, "no_stress"));
    const auto observed = std::string(schedule.name) == "no_stress"
                              ? baseline
                              : fuzz::execute_once(verified, schedule);
    require(baseline.ok && observed.ok,
            "strings2 grammar trapped seed=" + std::to_string(seed) +
                " schedule=" + schedule.name + " baseline=" + baseline.error +
                " observed=" + observed.error);
    require(!baseline.observable.empty() && !baseline.output.empty(),
            "strings2 grammar did not exercise both differential oracles");
    require(fuzz::same_observables(baseline, observed),
            "strings2 oracle drift seed=" + std::to_string(seed) +
                " schedule=" + schedule.name +
                "\nbaseline graph:\n" + baseline.observable +
                "\nobserved graph:\n" + observed.observable +
                "\nbaseline output bytes:\n" +
                fuzz::render_output_bytes(baseline.output) +
                "\nobserved output bytes:\n" +
                fuzz::render_output_bytes(observed.output));
}

std::string strings2_mutant(std::uint64_t seed, std::size_t mutant) {
    switch (mutant) {
    case 0:
        return "\"s" + std::to_string(seed) + "\" < " +
               std::to_string(seed) + "\n";
    case 1:
        return "\"abc\".sub(0)\n";
    case 2:
        return "\"abc\".sub(true, 2)\n";
    case 3:
        return "let s: str = \"abc\";\n"
               "let w: weak<str> = weak(s);\n"
               "w.get().sub(0, 1)\n";
    }
    throw std::runtime_error("strings2 mutant index out of range");
}

void require_strings2_mutant(std::uint64_t seed, std::size_t mutant) {
    const auto source = strings2_mutant(seed, mutant);
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "strings2 mutant unexpectedly compiled seed=" +
                                std::to_string(seed) + " mutant=" +
                                std::to_string(mutant) + "\n" + source);
    require(!compiled.diagnostics.empty(),
            "strings2 mutant rejection had no positioned diagnostic");
}

void strings2_grammar_pinned_snapshot() {
    const std::string expected = R"SRC(fn retain(value: str) -> fn() -> str {
  fn() -> str { value }
}

let raw: str = "aa-s150-tail";
let lo: i64 = 3;
let hi: i64 = 7;
let piece: str = raw.sub(lo, hi);
let joined: str = piece + "-x";
let ordered: bool = piece >= piece;
let length: i64 = joined.len;
let first: i64 = joined[0];
let table: map<str, str> = map<str, str>();
table[piece] = joined;
let render: fn() -> str = retain(piece);
print(piece);
print(joined.sub(0, piece.len));
print(to_str(length));
print(to_str(first));
print(to_str(ordered));
for key, value in table {
  if key >= piece {
    print(value.sub(piece.len, value.len));
    break;
  } else {
    print("unreachable");
  }
}
pair(render(), table["s150"])
)SRC";
    require(generate_strings2_source(kStrings2SnapshotSeed) == expected,
            "strings2 pinned snapshot changed for seed " +
                std::to_string(kStrings2SnapshotSeed));
}

void strings2_grammar_checks_all_schedules_both_oracles_and_mutants() {
    const auto all_schedules = fuzz::schedules();
    require(all_schedules.size() == 15,
            "strings2 grammar requires exactly fifteen shared schedules");
    for (std::uint64_t seed = kStrings2FirstSeed;
         seed < kStrings2FirstSeed + kStrings2CorpusSize; ++seed) {
        for (const auto& schedule : all_schedules) {
            run_strings2_seed_schedule(seed, schedule);
        }
    }
    for (std::uint64_t seed = kStrings2FirstSeed;
         seed < kStrings2FirstSeed + kStrings2MutantCorpusSize; ++seed) {
        for (std::size_t mutant = 0; mutant < 4; ++mutant) {
            require_strings2_mutant(seed, mutant);
        }
    }
}

void dump_strings2_corpus() {
    for (std::uint64_t seed = kStrings2FirstSeed;
         seed < kStrings2FirstSeed + kStrings2CorpusSize; ++seed) {
        std::cout << "grammar=strings2 seed=" << seed << "\n"
                  << generate_strings2_source(seed);
    }
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int run(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--dump-corpus" &&
        std::string(argv[2]) == "strings2") {
        dump_strings2_corpus();
        return 0;
    }
    if (argc == 7 &&
        (std::string(argv[1]) == "--grammar" ||
         std::string(argv[1]) == "--replay") &&
        std::string(argv[2]) == "strings2" &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--schedule") {
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto all_schedules = fuzz::schedules();
        run_strings2_seed_schedule(
            seed, fuzz::find_schedule(all_schedules, argv[6]));
        std::cerr << "[PASS] strings2 replay seed=" << seed
                  << " schedule=" << argv[6] << "\n";
        return 0;
    }
    if (argc == 7 &&
        (std::string(argv[1]) == "--grammar" ||
         std::string(argv[1]) == "--replay") &&
        std::string(argv[2]) == "strings2" &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--mutant") {
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto mutant = static_cast<std::size_t>(std::stoull(argv[6]));
        require_strings2_mutant(seed, mutant);
        std::cerr << "[PASS] strings2 mutant replay seed=" << seed
                  << " mutant=" << mutant << "\n";
        return 0;
    }
    if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " --dump-corpus strings2\n       " << argv[0]
                  << " <--grammar|--replay> strings2 --seed N --schedule NAME\n       "
                  << argv[0]
                  << " <--grammar|--replay> strings2 --seed N --mutant <0..3>\n";
        return 2;
    }

    const std::vector<TestCase> tests = {
        {"verifier_types_new_opcodes_and_marks_sub_source_as_a_root",
         verifier_types_new_opcodes_and_marks_sub_source_as_a_root},
        {"substring_trap_matrix_and_copy_identity_are_deterministic",
         substring_trap_matrix_and_copy_identity_are_deterministic},
        {"str_lt_is_unsigned_bytewise_lexicographic",
         str_lt_is_unsigned_bytewise_lexicographic},
        {"substring_source_survives_movement_and_allocating_copy",
         substring_source_survives_movement_and_allocating_copy},
        {"frontend_derives_all_string_ordering_operators",
         frontend_derives_all_string_ordering_operators},
        {"reversed_string_ordering_preserves_left_to_right_operand_evaluation",
         reversed_string_ordering_preserves_left_to_right_operand_evaluation},
        {"frontend_sub_composes_with_maps_output_closures_and_for_in_break",
         frontend_sub_composes_with_maps_output_closures_and_for_in_break},
        {"frontend_rejects_mixed_ordering_and_bad_sub_calls_with_positions",
         frontend_rejects_mixed_ordering_and_bad_sub_calls_with_positions},
        {"strings2_grammar_pinned_snapshot",
         strings2_grammar_pinned_snapshot},
        {"strings2_grammar_checks_all_schedules_both_oracles_and_mutants",
         strings2_grammar_checks_all_schedules_both_oracles_and_mutants},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << "\n" << error.what() << "\n";
        }
    }
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] iteration31 strings2 harness\n"
                  << error.what() << "\n";
        return 1;
    }
}
