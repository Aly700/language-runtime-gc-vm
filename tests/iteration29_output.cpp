#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"
#include "fuzz_common.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
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

std::string output_string(const lang::VM& vm) {
    return std::string(vm.output().begin(), vm.output().end());
}

std::string trap_for(const lang::VerifiedModule& module,
                     const fuzz::Schedule& schedule) {
    lang::VM vm;
    vm.set_gc_stress(schedule.stress);
    try {
        (void)vm.execute(module);
    } catch (const std::exception& error) {
        return error.what();
    }
    throw std::runtime_error("expected deterministic runtime trap");
}

lang::Module conversion_module(lang::OpCode conversion,
                               lang::ValueKind return_type,
                               std::int64_t integer = 0,
                               std::string string = {}) {
    lang::Module module;
    module.entry_function = 0;
    lang::Function function;
    function.signature.return_type = return_type;
    if (conversion == lang::OpCode::StrToI64) {
        module.string_constants.push_back(std::move(string));
        function.code.push_back({lang::OpCode::PushStr, 0});
    } else if (conversion == lang::OpCode::BoolToStr) {
        function.code.push_back({lang::OpCode::ConstantI64, integer ? 0 : 1});
        function.code.push_back({lang::OpCode::ConstantI64, integer ? 1 : 0});
        function.code.push_back({lang::OpCode::LessI64, 0});
    } else {
        function.code.push_back({lang::OpCode::ConstantI64, integer});
    }
    function.code.push_back({conversion, 0});
    function.code.push_back({lang::OpCode::Return, 0});
    module.functions.push_back(std::move(function));
    return module;
}

void verifier_types_all_four_opcodes_with_stable_reasons() {
    {
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Int64;
        function.code = {{lang::OpCode::ConstantI64, 1},
                         {lang::OpCode::Print, 0},
                         {lang::OpCode::ConstantI64, 0},
                         {lang::OpCode::Return, 0}};
        const auto report = lang::verify_with_diagnostics(function);
        require(!report.result.has_value() && !report.diagnostics.empty() &&
                    report.diagnostics.front().reason ==
                        lang::VerifierReason::PrintRequiresStr,
                "Print type confusion lacked its stable reason");
    }
    {
        lang::Module module;
        module.string_constants = {"x"};
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Str;
        function.code = {{lang::OpCode::PushStr, 0},
                         {lang::OpCode::I64ToStr, 0},
                         {lang::OpCode::Return, 0}};
        module.functions.push_back(std::move(function));
        const auto report = lang::verify_with_diagnostics(module);
        require(!report.result.has_value() && !report.diagnostics.empty() &&
                    report.diagnostics.front().reason ==
                        lang::VerifierReason::I64ToStrRequiresI64,
                "I64ToStr type confusion lacked its stable reason");
    }
    {
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Int64;
        function.code = {{lang::OpCode::ConstantI64, 1},
                         {lang::OpCode::StrToI64, 0},
                         {lang::OpCode::Return, 0}};
        const auto report = lang::verify_with_diagnostics(function);
        require(!report.result.has_value() && !report.diagnostics.empty() &&
                    report.diagnostics.front().reason ==
                        lang::VerifierReason::StrToI64RequiresStr,
                "StrToI64 type confusion lacked its stable reason");
    }
    {
        lang::Function function;
        function.signature.return_type = lang::ValueKind::Str;
        function.code = {{lang::OpCode::ConstantI64, 1},
                         {lang::OpCode::BoolToStr, 0},
                         {lang::OpCode::Return, 0}};
        const auto report = lang::verify_with_diagnostics(function);
        require(!report.result.has_value() && !report.diagnostics.empty() &&
                    report.diagnostics.front().reason ==
                        lang::VerifierReason::BoolToStrRequiresBool,
                "BoolToStr type confusion lacked its stable reason");
    }
}

void i64_to_str_forms_and_round_trips_are_canonical() {
    const std::vector<std::int64_t> values = {
        0,
        -1,
        1,
        -10,
        10,
        -9'876'543'210,
        9'876'543'210,
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max(),
    };
    for (const auto value : values) {
        auto module = conversion_module(lang::OpCode::I64ToStr,
                                        lang::ValueKind::Str, value);
        auto verified = test_support::verify_module_or_throw(
            std::move(module), "canonical I64ToStr module");
        for (const auto& schedule : fuzz::schedules()) {
            lang::VM vm;
            vm.set_gc_stress(schedule.stress);
            const auto result = vm.execute(verified);
            const auto bytes = vm.heap().string_bytes(result.as_object());
            require(std::string(bytes.begin(), bytes.end()) ==
                        std::to_string(value),
                    "I64ToStr was non-canonical under " +
                        std::string(schedule.name));
        }

        const std::string source = "to_i64(to_str(" +
                                   std::to_string(value) + "))\n";
        const auto compiled = require_compiles(source);
        lang::VM vm;
        require(vm.execute(*compiled.verified_module).as_i64() == value,
                "frontend conversion round-trip changed i64 value");
    }

    for (const bool value : {false, true}) {
        auto module = conversion_module(lang::OpCode::BoolToStr,
                                        lang::ValueKind::Str, value ? 1 : 0);
        auto verified = test_support::verify_module_or_throw(
            std::move(module), "canonical BoolToStr module");
        lang::VM vm;
        const auto result = vm.execute(verified);
        const auto bytes = vm.heap().string_bytes(result.as_object());
        require(std::string(bytes.begin(), bytes.end()) ==
                    (value ? "true" : "false"),
                "BoolToStr returned non-canonical text");
    }
}

void str_to_i64_acceptance_and_trap_matrix_is_deterministic() {
    for (const auto& [text, expected] :
         std::vector<std::pair<std::string, std::int64_t>>{
             {"0", 0},
             {"9223372036854775807", std::numeric_limits<std::int64_t>::max()},
             {"-9223372036854775808", std::numeric_limits<std::int64_t>::min()},
         }) {
        auto verified = test_support::verify_module_or_throw(
            conversion_module(lang::OpCode::StrToI64,
                              lang::ValueKind::Int64, 0, text),
            "accepted StrToI64 module");
        lang::VM vm;
        require(vm.execute(verified).as_i64() == expected,
                "StrToI64 rejected or changed canonical boundary value");
    }

    const std::vector<std::string> malformed = {
        "", "+1", " 1", "1 ", "01", "-0", "--1", "abc",
        "9223372036854775808", "-9223372036854775809",
    };
    for (const auto& text : malformed) {
        auto verified = test_support::verify_module_or_throw(
            conversion_module(lang::OpCode::StrToI64,
                              lang::ValueKind::Int64, 0, text),
            "malformed StrToI64 module");
        std::string baseline;
        for (const auto& schedule : fuzz::schedules()) {
            const auto observed = trap_for(verified, schedule);
            require(observed.find("invalid string for i64 conversion") !=
                        std::string::npos,
                    "StrToI64 used an unstable trap diagnostic");
            if (baseline.empty()) {
                baseline = observed;
            } else {
                require(observed == baseline,
                        "StrToI64 trap diagnostic/pc drifted across schedules");
            }
        }
    }
}

void output_cap_and_vm_reuse_are_deterministic() {
    lang::Module overflow;
    constexpr std::size_t kChunkBytes = 4095;
    static_assert(lang::VM::kMaxOutputBytes % (kChunkBytes + 1) == 0);
    overflow.string_constants = {std::string(kChunkBytes, 'x'), ""};
    lang::Function overflow_function;
    overflow_function.signature.return_type = lang::ValueKind::Int64;
    overflow_function.local_count = 1;
    overflow_function.code = {{lang::OpCode::PushStr, 0},
                              {lang::OpCode::StoreLocal, 0}};
    for (std::size_t i = 0;
         i < lang::VM::kMaxOutputBytes / (kChunkBytes + 1); ++i) {
        overflow_function.code.push_back({lang::OpCode::LoadLocal, 0});
        overflow_function.code.push_back({lang::OpCode::Print, 0});
    }
    overflow_function.code.push_back({lang::OpCode::PushStr, 1});
    overflow_function.code.push_back({lang::OpCode::Print, 0});
    overflow_function.code.push_back({lang::OpCode::ConstantI64, 0});
    overflow_function.code.push_back({lang::OpCode::Return, 0});
    overflow.functions.push_back(std::move(overflow_function));
    const auto verified_overflow = test_support::verify_module_or_throw(
        std::move(overflow), "output overflow module");
    std::string baseline;
    for (const auto& schedule : fuzz::schedules()) {
        const auto observed = trap_for(verified_overflow, schedule);
        require(observed.find("output buffer overflow") != std::string::npos,
                "output overflow used the wrong stable diagnostic");
        if (baseline.empty()) {
            baseline = observed;
        } else {
            require(observed == baseline,
                    "output overflow trap diagnostic/pc drifted across schedules");
        }
    }

    auto first = require_compiles("print(\"first\");\n0\n");
    auto second = require_compiles("print(\"second\");\n0\n");
    lang::VM vm;
    (void)vm.execute(*first.verified_module);
    require(output_string(vm) == "first\n", "first execution output was wrong");
    (void)vm.execute(*second.verified_module);
    require(output_string(vm) == "second\n",
            "VM reuse did not reset output between executions");
}

void frontend_prints_every_string_construction_path() {
    const std::string source = R"SRC(
fn captured(prefix: str) -> fn(str) -> str {
  fn(suffix: str) -> str { prefix + suffix }
}

let literal: str = "pool";
let joined: str = literal + "-concat";
let values: [str] = [joined];
let table: map<str, str> = map<str, str>();
table["map-key"] = values[0];
let close: fn(str) -> str = captured("closure-");
print(literal);
print(joined);
print(values[0]);
print(close("captured"));
for key, value in table {
  print(key);
  print(value);
}
pair(joined, table)
)SRC";
    const auto compiled = require_compiles(source);
    const std::string expected =
        "pool\npool-concat\npool-concat\nclosure-captured\n"
        "map-key\npool-concat\n";
    for (const auto& schedule : fuzz::schedules()) {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        (void)vm.execute(*compiled.verified_module);
        require(output_string(vm) == expected,
                "construction-path output drifted under " +
                    std::string(schedule.name));
    }
}

void output_under_maximum_gc_stress_is_byte_identical() {
    const std::string source = R"SRC(
let i: i64 = 0;
let prefix: str = "value=";
let moved: str = "seed";
while i < 32 {
  moved = prefix + to_str(i);
  print(moved);
  print(to_str(to_i64(to_str(i))));
  i = i + 1;
}
moved
)SRC";
    const auto compiled = require_compiles(source);
    std::string baseline;
    for (const auto& schedule : fuzz::schedules()) {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        (void)vm.execute(*compiled.verified_module);
        const auto observed = output_string(vm);
        if (baseline.empty()) {
            baseline = observed;
        } else {
            require(observed == baseline,
                    "crown-jewel output bytes drifted under " +
                        std::string(schedule.name));
        }
    }
    require(!baseline.empty(), "crown-jewel program produced no output");
}

void frontend_rejects_conversion_and_print_type_errors_with_positions() {
    require_diagnostic("print(1);\n0\n", 1, 7, "print expects str");
    require_diagnostic("to_i64(true)\n", 1, 8, "to_i64 expects str");
    require_diagnostic("to_str(\"x\")\n", 1, 8,
                       "to_str expects i64 or bool");
}

void frontend_rejects_unrefined_weak_strings_and_accepts_refinement() {
    require_diagnostic(R"SRC(let s: str = "x";
let w: weak<str> = weak(s);
print(w.get());
0
)SRC",
                       3, 9, "print requires non-nil str");
    require_diagnostic(R"SRC(let s: str = "x";
let w: weak<str> = weak(s);
to_i64(w.get())
)SRC",
                       3, 10, "to_i64 requires non-nil str");

    const auto compiled = require_compiles(R"SRC(let s: str = "42";
let w: weak<str> = weak(s);
let recovered: str = w.get();
if is_nil(recovered) {
  print("cleared");
} else {
  print(recovered);
}
0
)SRC");
    lang::VM vm;
    (void)vm.execute(*compiled.verified_module);
    require(output_string(vm) == "42\n",
            "is_nil false-branch refinement did not enable Print");
}

void output_failure_rendering_is_byte_safe() {
    const std::string bytes("A\n\0\\", 4);
    require(fuzz::render_output_bytes(bytes) == "41 0a 00 5c",
            "output failure rendering is ambiguous for control bytes");
    require(fuzz::render_output_bytes("").empty(),
            "empty output failure rendering was non-empty");
}

constexpr std::uint64_t kOutputSnapshotSeed = 29;
constexpr std::uint64_t kOutputCorpusFirstSeed = 1;
constexpr std::uint64_t kOutputCorpusSize = 32;
constexpr std::uint64_t kOutputMutantCorpusSize = 12;

std::string generate_output_source(std::uint64_t seed) {
    const auto base = static_cast<std::int64_t>((seed * 29) % 101) - 50;
    const auto delta = static_cast<std::int64_t>(seed % 17) + 1;
    const auto computed = base + delta;
    const auto limit = computed + 1;
    const auto key = std::string("key-") + std::to_string(seed % 23);

    std::ostringstream out;
    out << "fn capture(prefix: str) -> fn(str) -> str {\n"
        << "  fn(value: str) -> str { prefix + value }\n"
        << "}\n\n"
        << "let base: i64 = " << base << ";\n"
        << "let computed: i64 = base + " << delta << ";\n"
        << "let flag: bool = computed < " << limit << ";\n"
        << "let numbers: [i64] = [base, computed];\n"
        << "let table: map<str, i64> = map<str, i64>();\n"
        << "table[\"" << key << "\"] = numbers[1];\n"
        << "let render: fn(str) -> str = capture(\"capture-" << seed
        << ":\");\n"
        << "let rendered: str = render(to_str(to_i64(to_str(computed))));\n"
        << "print(\"base=\" + to_str(base));\n"
        << "print(\"flag=\" + to_str(flag));\n"
        << "print(\"array=\" + to_str(numbers[1]));\n"
        << "print(rendered);\n"
        << "for key, value in table {\n"
        << "  print(key + \"=\" + to_str(value));\n"
        << "}\n"
        << "pair(rendered, table)\n";
    return out.str();
}

lang::VerifiedModule compile_output_source(std::uint64_t seed) {
    const auto source = generate_output_source(seed);
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(), "output grammar rejected seed=" +
                               std::to_string(seed) + "\n" + source + "\n" +
                               diagnostics_listing(compiled.diagnostics));
    const auto report =
        lang::verify_with_diagnostics(compiled.verified_module->module());
    require(report.result.has_value(),
            "output grammar produced verifier-rejected bytecode seed=" +
                std::to_string(seed));
    return *compiled.verified_module;
}

void run_output_seed_schedule(std::uint64_t seed,
                              const fuzz::Schedule& schedule) {
    const auto verified = compile_output_source(seed);
    const auto all_schedules = fuzz::schedules();
    const auto baseline = fuzz::execute_once(
        verified, fuzz::find_schedule(all_schedules, "no_stress"));
    const auto observed = std::string(schedule.name) == "no_stress"
                              ? baseline
                              : fuzz::execute_once(verified, schedule);
    require(baseline.ok && observed.ok,
            "output grammar trapped seed=" + std::to_string(seed) +
                " schedule=" + schedule.name + " baseline=" + baseline.error +
                " observed=" + observed.error);
    require(!baseline.output.empty(),
            "output grammar failed to exercise the output oracle seed=" +
                std::to_string(seed));
    require(fuzz::same_observables(baseline, observed),
            "output grammar oracle drift seed=" + std::to_string(seed) +
                " schedule=" + schedule.name +
                "\nbaseline graph:\n" + baseline.observable +
                "\nobserved graph:\n" + observed.observable +
                "\nbaseline output bytes:\n" +
                fuzz::render_output_bytes(baseline.output) +
                "\nobserved output bytes:\n" +
                fuzz::render_output_bytes(observed.output));
}

std::string output_mutant(std::uint64_t seed, std::size_t mutant) {
    switch (mutant) {
    case 0:
        return "print(" + std::to_string(seed) + ");\n0\n";
    case 1:
        return "to_i64(true)\n";
    case 2:
        return "to_i64(\"01\")\n";
    case 3:
        return "to_i64(\"9223372036854775808\")\n";
    }
    throw std::runtime_error("output mutant index out of range");
}

void require_output_mutant(std::uint64_t seed, std::size_t mutant) {
    const auto source = output_mutant(seed, mutant);
    const auto compiled = lang::frontend::compile_program(source);
    if (mutant < 2) {
        require(!compiled.ok(), "output frontend mutant unexpectedly compiled seed=" +
                                    std::to_string(seed) + " mutant=" +
                                    std::to_string(mutant) + "\n" + source);
        return;
    }

    require(compiled.ok(), "runtime output mutant failed frontend compilation\n" +
                               source + "\n" +
                               diagnostics_listing(compiled.diagnostics));
    std::string baseline;
    for (const auto& schedule : fuzz::schedules()) {
        const auto observed = trap_for(*compiled.verified_module, schedule);
        require(observed.find("invalid string for i64 conversion") !=
                    std::string::npos,
                "runtime output mutant used wrong diagnostic");
        if (baseline.empty()) {
            baseline = observed;
        } else {
            require(observed == baseline,
                    "runtime output mutant trap pc/diagnostic drift seed=" +
                        std::to_string(seed) + " mutant=" +
                        std::to_string(mutant) + " schedule=" + schedule.name);
        }
    }
}

void output_grammar_pinned_snapshot() {
    const std::string expected = R"SRC(fn capture(prefix: str) -> fn(str) -> str {
  fn(value: str) -> str { prefix + value }
}

let base: i64 = -17;
let computed: i64 = base + 13;
let flag: bool = computed < -3;
let numbers: [i64] = [base, computed];
let table: map<str, i64> = map<str, i64>();
table["key-6"] = numbers[1];
let render: fn(str) -> str = capture("capture-29:");
let rendered: str = render(to_str(to_i64(to_str(computed))));
print("base=" + to_str(base));
print("flag=" + to_str(flag));
print("array=" + to_str(numbers[1]));
print(rendered);
for key, value in table {
  print(key + "=" + to_str(value));
}
pair(rendered, table)
)SRC";
    require(generate_output_source(kOutputSnapshotSeed) == expected,
            "output grammar pinned snapshot changed for seed " +
                std::to_string(kOutputSnapshotSeed));
}

void output_grammar_checks_both_oracles_and_mutants() {
    const auto all_schedules = fuzz::schedules();
    require(all_schedules.size() == 12,
            "output grammar requires exactly twelve shared schedules");
    for (std::uint64_t seed = kOutputCorpusFirstSeed;
         seed < kOutputCorpusFirstSeed + kOutputCorpusSize; ++seed) {
        for (const auto& schedule : all_schedules) {
            run_output_seed_schedule(seed, schedule);
        }
    }
    for (std::uint64_t seed = kOutputCorpusFirstSeed;
         seed < kOutputCorpusFirstSeed + kOutputMutantCorpusSize; ++seed) {
        for (std::size_t mutant = 0; mutant < 4; ++mutant) {
            require_output_mutant(seed, mutant);
        }
    }
}

void dump_output_corpus() {
    for (std::uint64_t seed = kOutputCorpusFirstSeed;
         seed < kOutputCorpusFirstSeed + kOutputCorpusSize; ++seed) {
        std::cout << "grammar=output seed=" << seed << "\n"
                  << generate_output_source(seed);
    }
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int run(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--dump-corpus" &&
        std::string(argv[2]) == "output") {
        dump_output_corpus();
        return 0;
    }
    if (argc == 7 && std::string(argv[1]) == "--grammar" &&
        std::string(argv[2]) == "output" &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--schedule") {
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto all_schedules = fuzz::schedules();
        run_output_seed_schedule(seed,
                                 fuzz::find_schedule(all_schedules, argv[6]));
        std::cerr << "[PASS] output replay seed=" << seed
                  << " schedule=" << argv[6] << "\n";
        return 0;
    }
    if (argc == 7 && std::string(argv[1]) == "--grammar" &&
        std::string(argv[2]) == "output" &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--mutant") {
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto mutant = static_cast<std::size_t>(std::stoull(argv[6]));
        require_output_mutant(seed, mutant);
        std::cerr << "[PASS] output mutant replay seed=" << seed
                  << " mutant=" << mutant << "\n";
        return 0;
    }
    if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " --dump-corpus output\n       " << argv[0]
                  << " --grammar output --seed N --schedule NAME\n       "
                  << argv[0]
                  << " --grammar output --seed N --mutant <0..3>\n";
        return 2;
    }

    const std::vector<TestCase> tests = {
        {"verifier_types_all_four_opcodes_with_stable_reasons",
         verifier_types_all_four_opcodes_with_stable_reasons},
        {"i64_to_str_forms_and_round_trips_are_canonical",
         i64_to_str_forms_and_round_trips_are_canonical},
        {"str_to_i64_acceptance_and_trap_matrix_is_deterministic",
         str_to_i64_acceptance_and_trap_matrix_is_deterministic},
        {"output_cap_and_vm_reuse_are_deterministic",
         output_cap_and_vm_reuse_are_deterministic},
        {"frontend_prints_every_string_construction_path",
         frontend_prints_every_string_construction_path},
        {"output_under_maximum_gc_stress_is_byte_identical",
         output_under_maximum_gc_stress_is_byte_identical},
        {"frontend_rejects_conversion_and_print_type_errors_with_positions",
         frontend_rejects_conversion_and_print_type_errors_with_positions},
        {"frontend_rejects_unrefined_weak_strings_and_accepts_refinement",
         frontend_rejects_unrefined_weak_strings_and_accepts_refinement},
        {"output_failure_rendering_is_byte_safe",
         output_failure_rendering_is_byte_safe},
        {"output_grammar_pinned_snapshot", output_grammar_pinned_snapshot},
        {"output_grammar_checks_both_oracles_and_mutants",
         output_grammar_checks_both_oracles_and_mutants},
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
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
