#include "lang/frontend/type_checker.hpp"
#include "lang/trace.hpp"
#include "lang/vm.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

struct Bundle {
    std::string events;
    std::string snapshots;
    std::string stats;
};

void validate_fixed_key_order(const Bundle& bundle) {
    if (bundle.events.find('\r') != std::string::npos ||
        bundle.snapshots.find('\r') != std::string::npos ||
        bundle.stats.find('\r') != std::string::npos) {
        throw std::runtime_error("trace bundle contains non-LF line endings");
    }
    std::istringstream lines(bundle.events);
    std::string line;
    while (std::getline(lines, line)) {
        const std::array<std::string_view, 10> keys{
            "{\"tick\":", ",\"seq\":", ",\"kind\":",
            ",\"id\":", ",\"size\":", ",\"gen\":",
            ",\"from\":", ",\"to\":", ",\"refs\":",
            ",\"src_pos\":"};
        std::size_t cursor = 0;
        for (const auto key : keys) {
            const auto position = line.find(key, cursor);
            if (position == std::string::npos || position < cursor) {
                throw std::runtime_error(
                    "event base keys are missing or out of order");
            }
            cursor = position + key.size();
        }
    }
    if (!bundle.events.ends_with('\n') ||
        !bundle.snapshots.ends_with('\n') ||
        !bundle.stats.ends_with('\n') ||
        !bundle.snapshots.starts_with("{\"tick\":0,\"seq\":0,\"live\":[") ||
        !bundle.stats.starts_with("{\"live_bytes_final\":")) {
        throw std::runtime_error(
            "trace bundle framing or fixed top-level key order drifted");
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "could not open trace artifact: " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string read_source(const std::string& name) {
    return read_file(std::filesystem::path(LANG_EXAMPLES_DIR) /
                     (name + ".lang"));
}

Bundle run_once(const lang::VerifiedModule& module,
                std::string_view schedule,
                const std::filesystem::path& directory) {
    lang::gc::StressConfig stress;
    if (!lang::configure_trace_schedule(schedule, stress)) {
        throw std::runtime_error(
            "trace determinism test requested an unknown schedule");
    }
    {
        lang::JsonlTraceWriter writer(directory, 17);
        lang::VM vm(&writer);
        vm.set_gc_stress(std::move(stress));
        (void)vm.execute(module);
    }
    Bundle bundle{
        read_file(directory / "events.jsonl"),
        read_file(directory / "snapshots.jsonl"),
        read_file(directory / "stats.json"),
    };
    if (bundle.events.empty() || bundle.snapshots.empty() ||
        bundle.stats.empty()) {
        throw std::runtime_error(
            "trace determinism fixture produced an empty stream");
    }
    validate_fixed_key_order(bundle);
    return bundle;
}

void require_equal(const Bundle& first, const Bundle& second,
                   std::string_view schedule) {
    if (first.events != second.events) {
        throw std::runtime_error(
            "events.jsonl is nondeterministic under " +
            std::string(schedule));
    }
    if (first.snapshots != second.snapshots) {
        throw std::runtime_error(
            "snapshots.jsonl is nondeterministic under " +
            std::string(schedule));
    }
    if (first.stats != second.stats) {
        throw std::runtime_error(
            "stats.json is nondeterministic under " +
            std::string(schedule));
    }
}

} // namespace

int main() {
    try {
        const auto compiled =
            lang::frontend::compile_program(read_source("string_builder"));
        if (!compiled.ok()) {
            throw std::runtime_error(
                compiled.diagnostics.empty()
                    ? "trace determinism fixture failed to compile"
                    : compiled.diagnostics.front().message);
        }

        const std::array<std::string_view, 3> schedules{
            "no_stress", "major_every_1", "combined_mark_compact"};
        const auto root =
            std::filesystem::current_path() /
            "trace_determinism_artifacts";
        for (const auto schedule : schedules) {
            const auto first = run_once(
                *compiled.verified_module, schedule,
                root / (std::string(schedule) + "_a"));
            const auto second = run_once(
                *compiled.verified_module, schedule,
                root / (std::string(schedule) + "_b"));
            require_equal(first, second, schedule);
            std::cerr << "[PASS] byte-identical trace under "
                      << schedule << "\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
