#include "lang/frontend/type_checker.hpp"
#include "lang/trace.hpp"
#include "lang/vm.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::filesystem::path program;
    std::string schedule;
    std::filesystem::path output_directory;
    std::size_t snapshot_interval{4096};
};

void print_usage(std::ostream& output) {
    output << "usage: lang_trace <program.lang> --schedule <name> --out <dir> "
              "[--snapshot-interval N]\n";
}

std::size_t parse_positive_size(const std::string& text) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(text, &consumed, 10);
    if (consumed != text.size() || parsed == 0 ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(
            "snapshot interval must be a positive integer");
    }
    return static_cast<std::size_t>(parsed);
}

Options parse_options(int argc, char** argv) {
    if (argc < 2) {
        throw std::invalid_argument("missing program path");
    }
    Options options;
    options.program = argv[1];
    bool saw_schedule = false;
    bool saw_output = false;
    bool saw_interval = false;
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (index + 1 >= argc) {
            throw std::invalid_argument(
                "missing value for option " + std::string(option));
        }
        const std::string value = argv[++index];
        if (option == "--schedule" && !saw_schedule) {
            options.schedule = value;
            saw_schedule = true;
        } else if (option == "--out" && !saw_output) {
            options.output_directory = value;
            saw_output = true;
        } else if (option == "--snapshot-interval" && !saw_interval) {
            options.snapshot_interval = parse_positive_size(value);
            saw_interval = true;
        } else {
            throw std::invalid_argument(
                "unknown or repeated option " + std::string(option));
        }
    }
    if (!saw_schedule || !saw_output) {
        throw std::invalid_argument(
            "both --schedule and --out are required");
    }
    return options;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "could not open source program: " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        throw std::runtime_error(
            "could not read source program: " + path.string());
    }
    return contents.str();
}

void write_json_string(std::ostream& output, std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    output.put('"');
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (byte < 0x20) {
                output << "\\u00" << kHex[(byte >> 4) & 0x0f]
                       << kHex[byte & 0x0f];
            } else {
                output.put(static_cast<char>(byte));
            }
            break;
        }
    }
    output.put('"');
}

void write_positions(const lang::Module& module,
                     const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "could not open positions.json for writing");
    }
    output << "{\"functions\":[";
    for (std::size_t function_index = 0;
         function_index < module.functions.size(); ++function_index) {
        if (function_index != 0) {
            output.put(',');
        }
        const auto& function = module.functions[function_index];
        output << "{\"index\":" << function_index << ",\"name\":";
        if (function.debug_name.empty()) {
            output << "null";
        } else {
            write_json_string(output, function.debug_name);
        }
        output << ",\"positions\":[";
        for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
            if (pc != 0) {
                output.put(',');
            }
            output << "{\"pc\":" << pc << ",\"source\":";
            if (pc < function.source_positions.size()) {
                output << "{\"line\":"
                       << function.source_positions[pc].line
                       << ",\"col\":"
                       << function.source_positions[pc].column << "}";
            } else {
                output << "null";
            }
            output.put('}');
        }
        output << "]}";
    }
    output << "]}\n";
    if (!output) {
        throw std::runtime_error("failed to write positions.json");
    }
}

void print_compile_diagnostics(
    const std::filesystem::path& path,
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << path.string() << ":" << diagnostic.position.line
                  << ":" << diagnostic.position.column << ": "
                  << diagnostic.message << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        try {
            options = parse_options(argc, argv);
        } catch (const std::exception& error) {
            std::cerr << "lang_trace: " << error.what() << "\n";
            print_usage(std::cerr);
            return 2;
        }

        lang::gc::StressConfig stress;
        if (!lang::configure_trace_schedule(options.schedule, stress)) {
            std::cerr << "lang_trace: unknown schedule '"
                      << options.schedule << "'. valid schedules:";
            for (const auto name : lang::trace_schedule_names()) {
                std::cerr << " " << name;
            }
            std::cerr << "\n";
            return 2;
        }

        const auto source = read_file(options.program);
        auto compiled = lang::frontend::compile_program(source);
        if (!compiled.ok()) {
            print_compile_diagnostics(options.program,
                                      compiled.diagnostics);
            return 1;
        }

        std::filesystem::create_directories(
            options.output_directory);
        write_positions(compiled.verified_module->module(),
                        options.output_directory / "positions.json");

        lang::JsonlTraceWriter writer(options.output_directory,
                                      options.snapshot_interval);
        lang::VM vm(&writer);
        vm.set_gc_stress(std::move(stress));

        int exit_code = 0;
        try {
            (void)vm.execute(*compiled.verified_module);
        } catch (const std::exception& error) {
            std::cerr << "lang_trace: " << error.what() << "\n";
            exit_code = 1;
        }
        const auto& bytes = vm.output();
        std::cout.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
        if (!std::cout) {
            throw std::runtime_error(
                "failed to forward VM output to stdout");
        }
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << "lang_trace: " << error.what() << "\n";
        return 1;
    }
}
