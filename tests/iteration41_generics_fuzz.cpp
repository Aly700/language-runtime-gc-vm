#include "fuzz_common.hpp"

#include "lang/frontend/type_checker.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t kFirstSeed = 1;
constexpr std::uint64_t kCorpusSize = 32;
constexpr std::uint64_t kSnapshotSeed = 41;
constexpr std::size_t kMutantCount = 12;
// `--dump-corpus generics` SHA-256:
// 8885efba70fb5788ae1486efd05453c46bf3a3e782bab73e801279b6778b350e

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ull;
    for (const auto byte : text) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 0x0000'0100'0000'01b3ull;
    }
    return hash;
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

std::string generate_source(std::uint64_t seed) {
    fuzz::SplitMix64 random(seed ^ 0x41A7'6E21'90D3'5B8Full);
    const auto first = random.small_i64() + 101;
    const auto second = random.small_i64() + 211;
    const auto huge =
        static_cast<std::int64_t>(4'294'967'296ull +
                                  random.bounded(4096));
    const auto code = random.small_i64() + 701;
    const auto steps =
        static_cast<std::int64_t>(random.bounded(7) + 1);
    const bool choose_first = random.bounded(2) == 0;
    const bool should_throw = random.bounded(2) == 0;
    const bool explicit_arrays = seed % 2 == 0;
    const bool explicit_closures = seed % 3 == 0;

    std::ostringstream out;
    out << "variant Error { Code(i64) }\n"
        << "record ScalarBox { payload: i64 }\n"
        << "record RefBox { payload: pair<i64, i64> }\n"
        << "record Bundle {\n"
        << "  scalars: [i64],\n"
        << "  refs: [pair<i64, i64>],\n"
        << "  scalar_pair: pair<i64, i64>,\n"
        << "  ref_pair: pair<pair<i64, i64>, pair<i64, i64>>,\n"
        << "  scalar_box: ScalarBox,\n"
        << "  ref_box: RefBox,\n"
        << "  scalar_getter: fn() -> i64,\n"
        << "  ref_getter: fn() -> pair<i64, i64>,\n"
        << "  nested: pair<[i64], map<i64, pair<i64, i64>>>,\n"
        << "  observer: weak<pair<i64, i64>>,\n"
        << "  entry: ephemeron<pair<i64, i64>, pair<i64, i64>>,\n"
        << "  bounced: pair<i64, i64>\n"
        << "}\n\n"
        << "fn id<T>(value: T) -> T { value }\n"
        << "fn singleton<T>(value: T) -> [T] { [value] }\n"
        << "fn duplicate<T>(value: T) -> pair<T, T> {\n"
        << "  pair(value, value)\n"
        << "}\n"
        << "fn capture<T>(value: T) -> fn() -> T {\n"
        << "  fn() -> T { value }\n"
        << "}\n"
        << "fn choose<T>(flag: bool, first_value: T, "
           "second_value: T) -> T {\n"
        << "  let answer: T = first_value;\n"
        << "  if flag { answer = first_value; } "
           "else { answer = second_value; }\n"
        << "  answer\n"
        << "}\n"
        << "fn maybe_fail<T>(value: T, should_throw: bool, "
           "code: i64) -> T {\n"
        << "  if should_throw { throw Error.Code(code); } else { }\n"
        << "  value\n"
        << "}\n"
        << "fn bounce<T>(count: i64, value: T) -> T {\n"
        << "  if count < 1 { } else {\n"
        << "    return tail bounce<T>(count + -1, value);\n"
        << "  }\n"
        << "  value\n"
        << "}\n"
        << "fn package<T, U>(first_value: T, second_value: U) -> "
           "pair<[T], map<i64, U>> {\n"
        << "  let values: [T] = [first_value];\n"
        << "  let table: map<i64, U> = map<i64, U>();\n"
        << "  table[0] = second_value;\n"
        << "  pair(values, table)\n"
        << "}\n\n"
        << "let huge: i64 = " << huge << ";\n"
        << "let primary: pair<i64, i64> = pair(" << first << ", "
        << second << ");\n";
    if (explicit_arrays) {
        out << "let scalars: [i64] = singleton<i64>(huge);\n"
            << "let refs: [pair<i64, i64>] = "
               "singleton<pair<i64, i64>>(primary);\n";
    } else {
        out << "let scalars: [i64] = singleton(huge);\n"
            << "let refs: [pair<i64, i64>] = singleton(primary);\n";
    }
    out << "let scalar_pair: pair<i64, i64> = duplicate(huge);\n"
        << "let ref_pair: pair<pair<i64, i64>, pair<i64, i64>> = "
           "duplicate<pair<i64, i64>>(primary);\n";
    if (explicit_closures) {
        out << "let scalar_getter: fn() -> i64 = capture<i64>(huge);\n"
            << "let ref_getter: fn() -> pair<i64, i64> = "
               "capture<pair<i64, i64>>(primary);\n";
    } else {
        out << "let scalar_getter: fn() -> i64 = capture(huge);\n"
            << "let ref_getter: fn() -> pair<i64, i64> = "
               "capture(primary);\n";
    }
    out << "let scalar_box: ScalarBox = "
           "id(ScalarBox { payload: huge });\n"
        << "let ref_box: RefBox = "
           "id<RefBox>(RefBox { payload: primary });\n"
        << "let repeated_ref_box: RefBox = id(ref_box);\n"
        << "let chosen: i64 = choose(" << (choose_first ? "true" : "false")
        << ", " << first << ", " << second << ");\n"
        << "let caught: i64 = 0;\n"
        << "try {\n"
        << "  caught = maybe_fail<i64>(chosen, "
        << (should_throw ? "true" : "false") << ", " << code << ");\n"
        << "} catch (error: Error) {\n"
        << "  match error {\n"
        << "    Code(caught_code) => { caught = caught_code; }\n"
        << "  }\n"
        << "}\n"
        << "let bounced: pair<i64, i64> = "
           "bounce<pair<i64, i64>>("
        << steps << ", primary);\n"
        << "let nested: pair<[i64], map<i64, pair<i64, i64>>> = "
           "package<i64, pair<i64, i64>>(chosen, bounced);\n"
        << "let observer: weak<pair<i64, i64>> = weak(primary);\n"
        << "let entry: ephemeron<pair<i64, i64>, pair<i64, i64>> = "
           "ephemeron(primary, bounced);\n"
        << "let scalar_observed: i64 = scalar_getter();\n"
        << "let ref_observed: pair<i64, i64> = ref_getter();\n"
        << "print(to_str(scalar_observed));\n"
        << "print(to_str(ref_observed.left));\n"
        << "print(to_str(caught));\n"
        << "if is_nil(repeated_ref_box) {\n"
        << "  print(\"nil-ref-box\");\n"
        << "} else {\n"
        << "  print(to_str(repeated_ref_box.payload.right));\n"
        << "}\n"
        << "Bundle { scalars: scalars, refs: refs, "
           "scalar_pair: scalar_pair, ref_pair: ref_pair, "
           "scalar_box: scalar_box, ref_box: repeated_ref_box, "
           "scalar_getter: scalar_getter, ref_getter: ref_getter, "
           "nested: nested, observer: observer, entry: entry, "
           "bounced: bounced }\n";
    return out.str();
}

std::string replay_command(std::uint64_t seed,
                           const fuzz::Schedule& schedule) {
    return "./build/lang_iteration41_generics_fuzz "
           "--grammar generics --seed " +
           std::to_string(seed) + " --schedule " + schedule.name;
}

lang::VerifiedModule compile_source(std::uint64_t seed,
                                    const std::string& source) {
    const auto compiled = lang::frontend::compile_program(source);
    require(
        compiled.ok() && compiled.verified_module.has_value(),
        "generics grammar rejected seed=" + std::to_string(seed) +
            "\nrepro: " +
            replay_command(seed,
                           fuzz::find_schedule(fuzz::schedules(),
                                               "no_stress")) +
            "\nsource:\n" + source + "\ndiagnostics:\n" +
            diagnostics_listing(compiled.diagnostics));
    const auto& verified = *compiled.verified_module;
    const auto report =
        lang::verify_with_diagnostics(verified.module());
    require(
        report.result.has_value(),
        "generics grammar violated compiler/verifier agreement seed=" +
            std::to_string(seed) + "\nsource:\n" + source);
    require(
        verified.module().functions.size() == 15 &&
            report.result->functions.size() == 15,
        "generics grammar emitted the wrong concrete function count seed=" +
            std::to_string(seed));
    require(
        verified.module().record_layouts.size() == 3 &&
            verified.module().record_layouts[0].reference_map ==
                std::vector<bool>{false} &&
            verified.module().record_layouts[1].reference_map ==
                std::vector<bool>{true},
        "generics grammar lost scalar/reference record precision seed=" +
            std::to_string(seed));
    require(
        verified.module().functions[1].signature.return_type ==
                lang::ValueKind::Array &&
            verified.module().functions[2].signature.return_type ==
                lang::ValueKind::Array,
        "generics grammar lost deterministic singleton first-use order seed=" +
            std::to_string(seed));
    return verified;
}

void compare_schedule(std::uint64_t seed, const std::string& source,
                      const lang::VerifiedModule& module,
                      const fuzz::Outcome& baseline,
                      const fuzz::Schedule& schedule) {
    const auto observed =
        std::string_view(schedule.name) == "no_stress"
            ? baseline
            : fuzz::execute_once(module, schedule);
    require(
        baseline.ok && observed.ok,
        "generics grammar trapped seed=" + std::to_string(seed) +
            " schedule=" + schedule.name +
            " baseline=" + baseline.error +
            " observed=" + observed.error + "\nrepro: " +
            replay_command(seed, schedule) + "\nsource:\n" + source);
    require(
        !baseline.observable.empty() && !baseline.output.empty(),
        "generics grammar skipped graph or output oracle seed=" +
            std::to_string(seed) + "\nrepro: " +
            replay_command(seed, schedule));
    require(
        fuzz::same_observables(baseline, observed),
        "generics oracle drift seed=" + std::to_string(seed) +
            " schedule=" + schedule.name + "\nrepro: " +
            replay_command(seed, schedule) + "\nsource:\n" + source +
            "\nbaseline graph:\n" + baseline.observable +
            "\nobserved graph:\n" + observed.observable +
            "\nbaseline output bytes:\n" +
            fuzz::render_output_bytes(baseline.output) +
            "\nobserved output bytes:\n" +
            fuzz::render_output_bytes(observed.output));
}

struct Mutant {
    std::string source;
    std::string expected;
};

Mutant mutant_source(std::uint64_t seed, std::size_t mutant) {
    const auto value =
        static_cast<std::int64_t>(seed % 31 + 1);
    switch (mutant) {
    case 0:
        return {
            "fn make<T>(seed: i64) -> T { seed }\n"
            "make(" +
                std::to_string(value) + ")\n",
            "cannot infer type argument 'T'"};
    case 1:
        return {
            "fn same<T>(first: T, second: T) -> T { first }\n"
            "same(1, true)\n",
            "cannot infer unambiguous type arguments"};
    case 2:
        return {
            "fn id<T>(value: T) -> T { value }\n"
            "id<i64, bool>(1)\n",
            "expects 1 type argument(s) but got 2"};
    case 3:
        return {
            "fn id<T>(value: T) -> T { value }\n"
            "let callable: fn(i64) -> i64 = id;\n"
            "callable(1)\n",
            "must be called with concrete type arguments"};
    case 4:
        return {
            "fn increment<T>(value: T) -> T { value + 1 }\n"
            "increment<bool>(true)\n",
            "operator '+' requires i64 operands"};
    case 5:
        return {
            "fn table<K, V>(value: V) -> map<K, V> {\n"
            "  let result: map<K, V> = map<K, V>();\n"
            "  result\n"
            "}\n"
            "table<pair<i64, i64>, i64>(1)\n",
            "map key type must be i64, bool, or str"};
    case 6:
        return {
            "fn observe<T>(value: T) -> weak<T> { weak(value) }\n"
            "observe<i64>(1)\n",
            "weak target type must be an object type"};
    case 7:
        return {
            "fn grow<T>(value: T) -> T {\n"
            "  let ignored: [T] = grow<[T]>([value]);\n"
            "  value\n"
            "}\n"
            "grow<i64>(1)\n",
            "generic instantiation depth limit of 32 exceeded"};
    case 8:
        return {
            "fn left_grow<T>(value: T) -> T {\n"
            "  let ignored: [T] = right_grow<[T]>([value]);\n"
            "  value\n"
            "}\n"
            "fn right_grow<T>(value: T) -> T {\n"
            "  let ignored: [T] = left_grow<[T]>([value]);\n"
            "  value\n"
            "}\n"
            "left_grow<i64>(1)\n",
            "generic instantiation depth limit of 32 exceeded"};
    case 9:
        return {
            "fn inferred_grow<T>(value: T) -> T {\n"
            "  let nested: [T] = [value];\n"
            "  let ignored: [T] = inferred_grow(nested);\n"
            "  value\n"
            "}\n"
            "inferred_grow(1)\n",
            "generic instantiation depth limit of 32 exceeded"};
    case 10:
        return {
            "fn map_grow<T>(value: T) -> T {\n"
            "  let table: map<i64, T> = map<i64, T>();\n"
            "  table[0] = value;\n"
            "  let ignored: map<i64, T> = "
            "map_grow<map<i64, T>>(table);\n"
            "  value\n"
            "}\n"
            "map_grow<i64>(1)\n",
            "generic instantiation depth limit of 32 exceeded"};
    case 11:
        return {
            "fn pair_up<T>(first: T, second: T) -> pair<T, T> {\n"
            "  pair(first, second)\n"
            "}\n"
            "pair_up<i64>(1)\n",
            "expects 2 argument(s) but got 1"};
    }
    throw std::out_of_range("generics mutant index");
}

void require_mutant_rejected(std::uint64_t seed,
                             std::size_t mutant) {
    const auto generated = mutant_source(seed, mutant);
    const auto compiled =
        lang::frontend::compile_program(generated.source);
    require(
        !compiled.ok() && !compiled.diagnostics.empty(),
        "generics mutant unexpectedly compiled seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant) +
            "\nrepro: ./build/lang_iteration41_generics_fuzz "
            "--grammar generics --seed " +
            std::to_string(seed) + " --mutant " +
            std::to_string(mutant) + "\nsource:\n" +
            generated.source);
    const auto expected = std::find_if(
        compiled.diagnostics.begin(), compiled.diagnostics.end(),
        [&](const lang::frontend::Diagnostic& diagnostic) {
            return diagnostic.message.find(generated.expected) !=
                   std::string::npos;
        });
    require(
        expected != compiled.diagnostics.end(),
        "generics mutant omitted stable diagnostic '" +
            generated.expected + "' seed=" + std::to_string(seed) +
            " mutant=" + std::to_string(mutant) +
            "\ndiagnostics:\n" +
            diagnostics_listing(compiled.diagnostics));
    require(
        std::all_of(
            compiled.diagnostics.begin(), compiled.diagnostics.end(),
            [&](const lang::frontend::Diagnostic& diagnostic) {
                return diagnostic.position.line > 0 &&
                       diagnostic.position.column > 0 &&
                       diagnostic.position.offset <
                           generated.source.size();
            }),
        "generics mutant rejection lacked a positioned diagnostic seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant));
}

void require_grammar(std::string_view grammar) {
    require(grammar == "generics", "expected generics grammar");
}

std::size_t parse_mutant(std::string_view text) {
    const auto parsed =
        fuzz::parse_seed(std::string(text));
    require(parsed < kMutantCount,
            "generics mutant index out of range");
    return static_cast<std::size_t>(parsed);
}

void pinned_snapshot(
    const std::vector<fuzz::Schedule>& schedules) {
    const auto source = generate_source(kSnapshotSeed);
    constexpr std::uint64_t kExpectedSourceHash =
        1'169'986'820'694'014'060ull;
    require(
        fnv1a64(source) == kExpectedSourceHash,
        "generics representative source pin changed: " +
            std::to_string(fnv1a64(source)));
    const auto module = compile_source(kSnapshotSeed, source);
    const auto outcome = fuzz::execute_once(
        module, fuzz::find_schedule(schedules, "no_stress"));
    require(outcome.ok,
            "generics representative source trapped: " +
                outcome.error);
    const auto combined =
        outcome.observable + "\nOUTPUT\n" + outcome.output;
    constexpr std::uint64_t kExpectedOutcomeHash =
        2'878'799'742'766'327'420ull;
    require(
        fnv1a64(combined) == kExpectedOutcomeHash,
        "generics representative outcome pin changed: " +
            std::to_string(fnv1a64(combined)));
}

int run(int argc, char** argv) {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "generics fuzz requires fifteen schedules");
    const auto& baseline_schedule =
        fuzz::find_schedule(schedules, "no_stress");

    if (argc == 7 &&
        (std::string_view(argv[1]) == "--grammar" ||
         std::string_view(argv[1]) == "--replay") &&
        std::string_view(argv[3]) == "--seed" &&
        std::string_view(argv[5]) == "--schedule") {
        require_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto source = generate_source(seed);
        const auto module = compile_source(seed, source);
        const auto baseline =
            fuzz::execute_once(module, baseline_schedule);
        const auto& schedule =
            fuzz::find_schedule(schedules, argv[6]);
        compare_schedule(seed, source, module, baseline, schedule);
        std::cerr << "[PASS] generics replay seed=" << seed
                  << " schedule=" << schedule.name << "\n";
        return 0;
    }
    if (argc == 7 &&
        (std::string_view(argv[1]) == "--grammar" ||
         std::string_view(argv[1]) == "--replay") &&
        std::string_view(argv[3]) == "--seed" &&
        std::string_view(argv[5]) == "--mutant") {
        require_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto mutant = parse_mutant(argv[6]);
        require_mutant_rejected(seed, mutant);
        std::cerr << "[PASS] generics mutant replay seed=" << seed
                  << " mutant=" << mutant << "\n";
        return 0;
    }
    if (argc == 3 &&
        std::string_view(argv[1]) == "--dump-corpus") {
        require_grammar(argv[2]);
        for (std::uint64_t seed = kFirstSeed;
             seed < kFirstSeed + kCorpusSize; ++seed) {
            std::cout << "===== seed " << seed << " =====\n"
                      << generate_source(seed);
        }
        return 0;
    }
    if (argc != 1) {
        std::cerr
            << "usage: " << argv[0]
            << " <--grammar|--replay> generics --seed N "
               "--schedule NAME\n"
            << "       " << argv[0]
            << " <--grammar|--replay> generics --seed N "
               "--mutant <0..11>\n"
            << "       " << argv[0]
            << " --dump-corpus generics\n";
        return 2;
    }

    pinned_snapshot(schedules);
    for (std::uint64_t seed = kFirstSeed;
         seed < kFirstSeed + kCorpusSize; ++seed) {
        const auto source = generate_source(seed);
        const auto module = compile_source(seed, source);
        const auto baseline =
            fuzz::execute_once(module, baseline_schedule);
        for (const auto& schedule : schedules) {
            compare_schedule(
                seed, source, module, baseline, schedule);
        }
        for (std::size_t mutant = 0; mutant < kMutantCount;
             ++mutant) {
            require_mutant_rejected(seed, mutant);
        }
    }
    std::cerr
        << "[PASS] generics_pinned_seed_snapshot seed="
        << kSnapshotSeed << "\n"
        << "[PASS] lang_iteration41_generics_fuzz seeds="
        << kCorpusSize << " schedules=" << schedules.size()
        << " executions=" << kCorpusSize * schedules.size()
        << " mutants=" << kCorpusSize * kMutantCount << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] iteration41 generics fuzz: "
                  << error.what() << "\n";
        return 1;
    }
}
