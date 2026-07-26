#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::array<const char*, 12> kExamples{
    "linked_list",
    "capture_accumulator",
    "word_frequency",
    "array_pipeline",
    "string_toolkit",
    "records_showcase",
    "variants_showcase",
    "generics_showcase",
    "generic_types_showcase",
    "string_interning",
    "diagnostics_showcase",
    "ergonomics_showcase",
};

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open example artifact: " + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
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

lang::gc::StressConfig maximum_stress() {
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    stress.collect_minor_after_every_write_barrier = true;
    return stress;
}

std::string render_runtime_failure(
    const std::exception& error,
    const std::optional<lang::RuntimeTrace>& trace) {
    if (!trace.has_value() || trace->frames.empty()) {
        throw std::runtime_error(
            "diagnostics showcase trap omitted its runtime trace");
    }
    std::ostringstream out;
    out << "error: " << error.what() << "\n";
    for (std::size_t i = 0; i < trace->frames.size(); ++i) {
        const auto& frame = trace->frames[i];
        out << "#" << i << " function=" << frame.function_index << " name=";
        if (frame.function_name.has_value()) {
            out << *frame.function_name;
        } else {
            out << "<unknown>";
        }
        out << " pc=" << frame.pc << " source=";
        if (frame.source_position.has_value()) {
            out << frame.source_position->line << ":"
                << frame.source_position->column;
        } else {
            out << "<unknown>";
        }
        out << "\n";
    }
    return out.str();
}

void run_example(const std::string& name) {
    const std::string base = std::string(LANG_EXAMPLES_DIR) + "/" + name;
    const auto source = read_file(base + ".lang");
    const auto expected = read_file(base + ".expected");
    const auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        throw std::runtime_error("example failed to compile: " + name + "\n" +
                                 diagnostics_listing(compiled.diagnostics));
    }

    const std::array<std::pair<const char*, lang::gc::StressConfig>, 2> schedules{
        std::pair{"default", lang::gc::StressConfig{}},
        std::pair{"maximum_stress", maximum_stress()},
    };
    for (const auto& [schedule_name, stress] : schedules) {
        lang::VM vm;
        vm.set_gc_stress(stress);
        std::string actual;
        if (name == "diagnostics_showcase") {
            bool trapped = false;
            try {
                (void)vm.execute(*compiled.verified_module);
            } catch (const std::exception& error) {
                trapped = true;
                actual = render_runtime_failure(
                    error, vm.last_trap_trace());
            }
            if (!trapped) {
                throw std::runtime_error(
                    "diagnostics showcase unexpectedly succeeded");
            }
        } else {
            (void)vm.execute(*compiled.verified_module);
            actual.assign(vm.output().begin(), vm.output().end());
        }
        if (actual != expected) {
            throw std::runtime_error(
                "example output mismatch: " + name + " schedule=" + schedule_name +
                "\nexpected bytes:\n" + expected + "\nactual bytes:\n" + actual);
        }
    }
    std::cerr << "[PASS] " << name << " default+maximum_stress\n";
}

} // namespace

int main() {
    try {
        for (const auto* example : kExamples) {
            run_example(example);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
