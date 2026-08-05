#include "lang/gc/heap.hpp"
#include "lang/trace.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open trace stats: " +
                                 path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

} // namespace

int main() {
    try {
        lang::gc::Heap heap;
        (void)heap.allocate_pair(lang::Value::int64(1),
                                 lang::Value::int64(2));
        heap.collect();

        const auto directory =
            std::filesystem::current_path() /
            "trace_writer_run_metrics_artifacts";
        lang::JsonlTraceWriter writer(directory, 8);
        heap.set_trace_sink(&writer);
        writer.on_program_start(heap);

        (void)heap.allocate_pair(lang::Value::int64(3),
                                 lang::Value::int64(4));
        heap.collect();
        writer.on_program_exit(heap, 0);
        heap.set_trace_sink(nullptr);

        const auto stats = read_file(directory / "stats.json");
        if (stats.find(
                "\"collection_count\":1,\"event_totals\"") ==
            std::string::npos) {
            throw std::runtime_error(
                "stats did not report the run-local collection delta");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
