#include "lang/frontend/type_checker.hpp"
#include "fuzz_common.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using fuzz::Outcome;
using fuzz::Schedule;
using fuzz::execute_once;
using fuzz::find_schedule;
using fuzz::parse_seed;
using fuzz::schedules;

constexpr std::uint64_t kSnapshotSeed = 17;
constexpr std::uint64_t kFirstCorpusSeed = 1;
constexpr std::uint64_t kPositiveCorpusSize = 48;
constexpr std::uint64_t kMutantCorpusSize = 12;
constexpr std::size_t kMutantCount = 3;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string source_listing(const std::string& source) {
    std::ostringstream out;
    out << "source:\n" << source;
    if (source.empty() || source.back() != '\n') {
        out << "\n";
    }
    return out.str();
}

std::string diagnostics_listing(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.position.line << ":" << diagnostic.position.column << " "
            << diagnostic.message << "\n";
    }
    return out.str();
}

struct GenType {
    std::string text;
};

GenType i64_type() { return GenType{"i64"}; }
GenType bool_type() { return GenType{"bool"}; }
GenType bare_pair_type() { return GenType{"pair"}; }

bool operator==(const GenType& lhs, const GenType& rhs) {
    return lhs.text == rhs.text;
}

bool is_pair_type(const GenType& type) {
    return type.text == "pair" || type.text.rfind("pair<", 0) == 0;
}

GenType pair_type(const GenType& left, const GenType& right) {
    return GenType{"pair<" + left.text + ", " + right.text + ">"};
}

bool conforms_to(const GenType& value, const GenType& target) {
    if (target.text == "pair") {
        return is_pair_type(value);
    }
    return value == target;
}

struct Expr {
    std::string text;
    GenType type;
};

Expr int_literal(std::int64_t value) {
    return Expr{std::to_string(value), i64_type()};
}

Expr bool_literal(bool value) {
    return Expr{value ? "true" : "false", bool_type()};
}

Expr add_expr(Expr left, Expr right) {
    require(left.type == i64_type() && right.type == i64_type(),
            "generator attempted non-i64 addition");
    return Expr{left.text + " + " + right.text, i64_type()};
}

Expr less_expr(Expr left, Expr right) {
    require(left.type == i64_type() && right.type == i64_type(),
            "generator attempted non-i64 comparison");
    return Expr{left.text + " < " + right.text, bool_type()};
}

Expr pair_expr(Expr left, Expr right) {
    return Expr{"pair(" + left.text + ", " + right.text + ")",
                pair_type(left.type, right.type)};
}

Expr field_expr(Expr receiver, const std::string& field, GenType result_type) {
    require(is_pair_type(receiver.type), "generator attempted field read on non-pair");
    return Expr{receiver.text + "." + field, std::move(result_type)};
}

struct FunctionSig {
    std::string name;
    std::vector<GenType> parameters;
    GenType result;
};

Expr call_expr(const FunctionSig& signature, std::vector<Expr> arguments) {
    require(arguments.size() == signature.parameters.size(),
            "generator attempted call with wrong arity");
    std::ostringstream text;
    text << signature.name << "(";
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        require(conforms_to(arguments[i].type, signature.parameters[i]),
                "generator attempted call with wrong argument type");
        if (i != 0) {
            text << ", ";
        }
        text << arguments[i].text;
    }
    text << ")";
    return Expr{text.str(), signature.result};
}

struct Binding {
    std::string name;
    GenType type;
};

class Env {
public:
    void declare(std::string name, GenType type) {
        require(!find(name).has_value(), "generator redeclared local '" + name + "'");
        bindings_.push_back(Binding{std::move(name), std::move(type)});
    }

    [[nodiscard]] Expr variable(const std::string& name) const {
        const auto type = find(name);
        require(type.has_value(), "generator referenced missing local '" + name + "'");
        return Expr{name, *type};
    }

    [[nodiscard]] GenType type_of(const std::string& name) const {
        const auto type = find(name);
        require(type.has_value(), "generator referenced missing local '" + name + "'");
        return *type;
    }

private:
    [[nodiscard]] std::optional<GenType> find(const std::string& name) const {
        for (const auto& binding : bindings_) {
            if (binding.name == name) {
                return binding.type;
            }
        }
        return std::nullopt;
    }

    std::vector<Binding> bindings_;
};

void emit_let(std::ostringstream& out, Env& env, const std::string& name,
              const GenType& type, const Expr& initializer) {
    require(conforms_to(initializer.type, type),
            "generator let type mismatch for '" + name + "'");
    out << "let " << name << ": " << type.text << " = " << initializer.text << ";\n";
    env.declare(name, type);
}

void emit_assign(std::ostringstream& out, const Env& env, const std::string& name,
                 const Expr& value) {
    require(conforms_to(value.type, env.type_of(name)),
            "generator assignment type mismatch for '" + name + "'");
    out << name << " = " << value.text << ";\n";
}

void emit_field_assign(std::ostringstream& out, Expr receiver, const std::string& field,
                       const GenType& field_type, Expr value) {
    require(is_pair_type(receiver.type), "generator field assignment on non-pair");
    require(conforms_to(value.type, field_type),
            "generator field assignment type mismatch");
    out << receiver.text << "." << field << " = " << value.text << ";\n";
}

std::int64_t nonzero_small(fuzz::SplitMix64& rng) {
    auto value = rng.small_i64();
    if (value == 0) {
        value = 1;
    }
    return value;
}

struct GeneratedProgram {
    std::uint64_t seed{0};
    std::size_t function_count{0};
    std::int64_t loop_limit{0};
    std::int64_t recursion_depth{0};
    std::string source;
};

GeneratedProgram generate_source_program(std::uint64_t seed) {
    fuzz::SplitMix64 rng(seed ^ 0x5011'CE5E'ED10'0001ull);

    const auto pair_pair_type = pair_type(bare_pair_type(), bare_pair_type());
    const auto leaf_type = pair_type(i64_type(), bool_type());
    const auto nested_type = pair_type(leaf_type, pair_pair_type);

    const auto function_count = static_cast<std::size_t>(2 + (seed % 4));
    const auto loop_limit = static_cast<std::int64_t>(1 + rng.bounded(3));
    const auto recursion_depth = static_cast<std::int64_t>(1 + rng.bounded(3));
    const auto flag_threshold = static_cast<std::int64_t>(1 + rng.bounded(4));
    const auto leaf_delta = nonzero_small(rng);
    const auto nudge_delta = nonzero_small(rng);
    const auto nudge_else_delta = -nonzero_small(rng);

    const FunctionSig make_leaf{"make_leaf", {i64_type(), bool_type()}, leaf_type};
    const FunctionSig rec{"rec",
                          {i64_type(), bare_pair_type(), pair_pair_type},
                          pair_pair_type};
    const FunctionSig read_leaf{"read_leaf", {leaf_type}, i64_type()};
    const FunctionSig choose_pair{"choose_pair",
                                  {bool_type(), pair_pair_type, pair_pair_type},
                                  pair_pair_type};
    const FunctionSig nudge{"nudge", {bool_type(), i64_type()}, i64_type()};

    std::ostringstream out;
    out << "fn make_leaf(x: i64, flag: bool) -> " << leaf_type.text << " {\n";
    out << "  pair(x + " << leaf_delta << ", flag)\n";
    out << "}\n\n";

    out << "fn rec(n: i64, tail: pair, anchor: " << pair_pair_type.text
        << ") -> " << pair_pair_type.text << " {\n";
    out << "  if n < 1 {\n";
    out << "    anchor.left = tail;\n";
    out << "  } else {\n";
    out << "    anchor.right = rec(n + -1, tail, anchor);\n";
    out << "  }\n";
    out << "  anchor\n";
    out << "}\n\n";

    if (function_count >= 3) {
        out << "fn read_leaf(p: " << leaf_type.text << ") -> i64 {\n";
        out << "  p.left\n";
        out << "}\n\n";
    }

    if (function_count >= 4) {
        out << "fn choose_pair(flag: bool, a: " << pair_pair_type.text
            << ", b: " << pair_pair_type.text << ") -> " << pair_pair_type.text
            << " {\n";
        out << "  if flag {\n";
        out << "    a.left = b;\n";
        out << "  } else {\n";
        out << "    b.right = a;\n";
        out << "  }\n";
        out << "  a\n";
        out << "}\n\n";
    }

    if (function_count >= 5) {
        out << "fn nudge(flag: bool, x: i64) -> i64 {\n";
        out << "  if flag {\n";
        out << "    x = x + " << nudge_delta << ";\n";
        out << "  } else {\n";
        out << "    x = x + " << nudge_else_delta << ";\n";
        out << "  }\n";
        out << "  x\n";
        out << "}\n\n";
    }

    Env env;
    emit_let(out, env, "atom", bare_pair_type(),
             pair_expr(int_literal(rng.small_i64()), int_literal(rng.small_i64())));
    emit_let(out, env, "seed", bare_pair_type(),
             pair_expr(env.variable("atom"), env.variable("atom")));
    emit_let(out, env, "current", pair_pair_type,
             pair_expr(env.variable("seed"), env.variable("seed")));
    emit_let(out, env, "flag", bool_type(),
             less_expr(int_literal(rng.small_i64()), int_literal(rng.small_i64())));
    emit_let(out, env, "leaf", leaf_type,
             call_expr(make_leaf, {int_literal(rng.small_i64()), env.variable("flag")}));
    emit_let(out, env, "nested", nested_type,
             pair_expr(env.variable("leaf"), env.variable("current")));
    emit_let(out, env, "rec_depth", i64_type(), int_literal(recursion_depth));
    emit_let(out, env, "i", i64_type(), int_literal(0));
    emit_let(out, env, "sum", i64_type(), int_literal(0));

    out << "while " << less_expr(env.variable("i"), int_literal(loop_limit)).text
        << " {\n";
    out << "  ";
    emit_field_assign(out, env.variable("current"), "left", bare_pair_type(),
                      pair_expr(env.variable("i"), env.variable("current")));
    out << "  ";
    emit_field_assign(out, env.variable("current"), "right", bare_pair_type(),
                      call_expr(rec, {env.variable("rec_depth"), env.variable("current"),
                                      env.variable("current")}));
    const auto leaf_read = function_count >= 3
                               ? call_expr(read_leaf, {env.variable("leaf")})
                               : field_expr(env.variable("leaf"), "left", i64_type());
    out << "  ";
    emit_assign(out, env, "sum", add_expr(env.variable("sum"), leaf_read));
    out << "  ";
    emit_assign(out, env, "flag",
                less_expr(env.variable("i"), int_literal(flag_threshold)));
    out << "  if flag {\n";
    out << "    ";
    emit_field_assign(out, env.variable("current"), "left", bare_pair_type(),
                      pair_expr(env.variable("leaf"), env.variable("current")));
    out << "  } else {\n";
    out << "    ";
    emit_field_assign(out, env.variable("current"), "right", bare_pair_type(),
                      env.variable("current"));
    out << "  }\n";
    out << "  ";
    emit_assign(out, env, "i", add_expr(env.variable("i"), int_literal(1)));
    out << "}\n";

    emit_assign(out, env, "current",
                call_expr(rec, {env.variable("rec_depth"), env.variable("current"),
                                env.variable("current")}));
    if (function_count >= 4) {
        emit_assign(out, env, "current",
                    call_expr(choose_pair, {env.variable("flag"), env.variable("current"),
                                            env.variable("current")}));
    }
    if (function_count >= 5) {
        emit_assign(out, env, "sum",
                    call_expr(nudge, {env.variable("flag"), env.variable("sum")}));
    }

    switch (seed % 4) {
    case 0:
        out << "current\n";
        break;
    case 1:
        out << "sum\n";
        break;
    case 2:
        out << "leaf.left\n";
        break;
    default:
        out << "flag\n";
        break;
    }

    return GeneratedProgram{seed, function_count, loop_limit, recursion_depth, out.str()};
}

lang::frontend::CompileResult require_compiles(const GeneratedProgram& generated) {
    auto compiled = lang::frontend::compile_program(generated.source);
    if (!compiled.ok()) {
        std::ostringstream out;
        out << "source generator emitted rejected program"
            << " seed=" << generated.seed
            << " functions=" << generated.function_count
            << " loop_limit=" << generated.loop_limit
            << " recursion_depth=" << generated.recursion_depth << "\n"
            << source_listing(generated.source)
            << "diagnostics:\n" << diagnostics_listing(compiled.diagnostics);
        throw std::runtime_error(out.str());
    }
    require(compiled.module.has_value(),
            "successful source compile did not return a module\n" +
                source_listing(generated.source));
    return compiled;
}

std::string repro_command(std::uint64_t seed, const char* schedule_name) {
    std::ostringstream out;
    out << "./build/lang_iteration10_source_fuzz --seed " << seed
        << " --schedule " << schedule_name;
    return out.str();
}

std::string mutant_repro_command(std::uint64_t seed, std::size_t mutant_index) {
    std::ostringstream out;
    out << "./build/lang_iteration10_source_fuzz --seed " << seed
        << " --mutant " << mutant_index;
    return out.str();
}

[[noreturn]] void report_failure(const GeneratedProgram& generated,
                                 const Schedule& schedule,
                                 const Outcome& baseline,
                                 const Outcome& observed) {
    std::ostringstream out;
    out << "source-level differential fuzz failure\n";
    out << "seed=" << generated.seed << " schedule=" << schedule.name
        << " functions=" << generated.function_count
        << " loop_limit=" << generated.loop_limit
        << " recursion_depth=" << generated.recursion_depth << "\n";
    out << "repro: " << repro_command(generated.seed, schedule.name) << "\n";
    out << source_listing(generated.source);
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
    const auto generated = generate_source_program(seed);
    const auto compiled = require_compiles(generated);
    const auto all_schedules = schedules();
    const auto& baseline_schedule = find_schedule(all_schedules, "no_stress");
    const auto baseline = execute_once(*compiled.module, baseline_schedule);
    const auto observed = schedule.name == std::string("no_stress")
                              ? baseline
                              : execute_once(*compiled.module, schedule);

    if (!baseline.ok || !observed.ok || baseline.observable != observed.observable) {
        report_failure(generated, schedule, baseline, observed);
    }
}

void run_seed_all_schedules(std::uint64_t seed,
                            const std::vector<Schedule>& all_schedules) {
    const auto generated = generate_source_program(seed);
    const auto compiled = require_compiles(generated);
    const auto baseline = execute_once(*compiled.module, all_schedules.front());
    for (const auto& schedule : all_schedules) {
        const auto observed = schedule.name == std::string(all_schedules.front().name)
                                  ? baseline
                                  : execute_once(*compiled.module, schedule);
        if (!baseline.ok || !observed.ok || baseline.observable != observed.observable) {
            report_failure(generated, schedule, baseline, observed);
        }
    }
}

std::string replace_once(std::string source, const std::string& needle,
                         const std::string& replacement,
                         const std::string& context) {
    const auto position = source.find(needle);
    require(position != std::string::npos,
            "mutator could not find " + context + " needle '" + needle + "'");
    source.replace(position, needle.size(), replacement);
    return source;
}

std::string mutate_source(const GeneratedProgram& generated,
                          std::size_t mutant_index) {
    switch (mutant_index) {
    case 0:
        return replace_once(generated.source, "sum = sum + ", "sum = sum < ",
                            "operator flip");
    case 1:
        return replace_once(generated.source, "sum = sum + ",
                            "sum = missing_sum + ", "undefined variable");
    case 2:
        return replace_once(generated.source,
                            "rec(rec_depth, current, current)",
                            "rec(rec_depth, current)", "call arity");
    default:
        throw std::runtime_error("unknown mutant index " +
                                 std::to_string(mutant_index));
    }
}

void require_mutant_rejected(std::uint64_t seed, std::size_t mutant_index) {
    const auto generated = generate_source_program(seed);
    const auto mutant = mutate_source(generated, mutant_index);
    const auto compiled = lang::frontend::compile_program(mutant);
    if (compiled.ok() || compiled.diagnostics.empty()) {
        std::ostringstream out;
        out << "source mutant unexpectedly compiled"
            << " seed=" << seed << " mutant=" << mutant_index << "\n";
        out << "repro: " << mutant_repro_command(seed, mutant_index) << "\n";
        out << source_listing(mutant);
        if (compiled.diagnostics.empty()) {
            out << "diagnostics: <none>\n";
        } else {
            out << "diagnostics:\n" << diagnostics_listing(compiled.diagnostics);
        }
        throw std::runtime_error(out.str());
    }
}

void run_mutant_corpus() {
    for (std::uint64_t seed = kFirstCorpusSeed;
         seed < kFirstCorpusSeed + kMutantCorpusSize; ++seed) {
        for (std::size_t mutant = 0; mutant < kMutantCount; ++mutant) {
            require_mutant_rejected(seed, mutant);
        }
    }
}

void pinned_source_snapshot() {
    const auto generated = generate_source_program(kSnapshotSeed);
    const std::string expected = R"SRC(fn make_leaf(x: i64, flag: bool) -> pair<i64, bool> {
  pair(x + -31, flag)
}

fn rec(n: i64, tail: pair, anchor: pair<pair, pair>) -> pair<pair, pair> {
  if n < 1 {
    anchor.left = tail;
  } else {
    anchor.right = rec(n + -1, tail, anchor);
  }
  anchor
}

fn read_leaf(p: pair<i64, bool>) -> i64 {
  p.left
}

let atom: pair = pair(26, 22);
let seed: pair = pair(atom, atom);
let current: pair<pair, pair> = pair(seed, seed);
let flag: bool = 5 < 35;
let leaf: pair<i64, bool> = make_leaf(-24, flag);
let nested: pair<pair<i64, bool>, pair<pair, pair>> = pair(leaf, current);
let rec_depth: i64 = 1;
let i: i64 = 0;
let sum: i64 = 0;
while i < 2 {
  current.left = pair(i, current);
  current.right = rec(rec_depth, current, current);
  sum = sum + read_leaf(leaf);
  flag = i < 3;
  if flag {
    current.left = pair(leaf, current);
  } else {
    current.right = current;
  }
  i = i + 1;
}
current = rec(rec_depth, current, current);
sum
)SRC";
    require(generated.source == expected,
            "source generator snapshot changed for seed " +
                std::to_string(kSnapshotSeed) + "\nexpected:\n" + expected +
                "actual:\n" + generated.source);
}

std::size_t parse_mutant_index(const std::string& value) {
    std::size_t parsed = 0;
    const auto index = std::stoull(value, &parsed, 10);
    if (parsed != value.size() || index >= kMutantCount) {
        throw std::runtime_error("invalid mutant index: " + value);
    }
    return static_cast<std::size_t>(index);
}

int run(int argc, char** argv) {
    const auto all_schedules = schedules();

    if (argc == 5 && std::string(argv[1]) == "--seed" &&
        std::string(argv[3]) == "--schedule") {
        const auto seed = parse_seed(argv[2]);
        const auto& schedule = find_schedule(all_schedules, argv[4]);
        run_seed_schedule(seed, schedule);
        std::cerr << "[PASS] source replay seed=" << seed
                  << " schedule=" << schedule.name << "\n";
        return 0;
    }

    if (argc == 5 && std::string(argv[1]) == "--seed" &&
        std::string(argv[3]) == "--mutant") {
        const auto seed = parse_seed(argv[2]);
        const auto mutant = parse_mutant_index(argv[4]);
        require_mutant_rejected(seed, mutant);
        std::cerr << "[PASS] source mutant replay seed=" << seed
                  << " mutant=" << mutant << "\n";
        return 0;
    }

    if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " [--seed <uint64> --schedule <schedule-name>]\n"
                  << "       " << argv[0]
                  << " --seed <uint64> --mutant <0.." << (kMutantCount - 1)
                  << ">\n";
        std::cerr << "schedules:";
        for (const auto& schedule : all_schedules) {
            std::cerr << " " << schedule.name;
        }
        std::cerr << "\n";
        return 2;
    }

    pinned_source_snapshot();

    for (std::uint64_t seed = kFirstCorpusSeed;
         seed < kFirstCorpusSeed + kPositiveCorpusSize; ++seed) {
        run_seed_all_schedules(seed, all_schedules);
    }
    run_mutant_corpus();

    std::cerr << "[PASS] source_pinned_seed_snapshot seed=" << kSnapshotSeed << "\n";
    std::cerr << "[PASS] lang_iteration10_source_fuzz positive seeds="
              << kPositiveCorpusSize << " schedules=" << all_schedules.size()
              << " executions=" << (kPositiveCorpusSize * all_schedules.size())
              << "\n";
    std::cerr << "[PASS] lang_iteration10_source_fuzz mutants seeds="
              << kMutantCorpusSize << " mutants=" << kMutantCount
              << " checks=" << (kMutantCorpusSize * kMutantCount) << "\n";
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
