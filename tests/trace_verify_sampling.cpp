#include "lang/gc/heap.hpp"
#include "lang/trace.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

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

std::vector<std::string> read_lines(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "could not open trace artifact: " + path.string());
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void require_contains(std::string_view text, std::string_view token,
                      std::string_view message) {
    require(text.find(token) != std::string_view::npos, message);
}

std::uint64_t integer_field(std::string_view text,
                            std::string_view name) {
    const auto prefix = '"' + std::string(name) + "\":";
    const auto field = text.find(prefix);
    if (field == std::string_view::npos) {
        throw std::runtime_error(
            "missing integer field: " + std::string(name));
    }
    const auto first = field + prefix.size();
    std::uint64_t value = 0;
    const auto result = std::from_chars(
        text.data() + first, text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr == text.data() + first) {
        throw std::runtime_error(
            "invalid integer field: " + std::string(name));
    }
    return value;
}

std::string string_field(std::string_view text,
                         std::string_view name) {
    const auto prefix = '"' + std::string(name) + "\":\"";
    const auto field = text.find(prefix);
    if (field == std::string_view::npos) {
        throw std::runtime_error(
            "missing string field: " + std::string(name));
    }
    const auto first = field + prefix.size();
    const auto last = text.find('"', first);
    if (last == std::string_view::npos) {
        throw std::runtime_error(
            "unterminated string field: " + std::string(name));
    }
    return std::string(text.substr(first, last - first));
}

void emit_verify(lang::JsonlTraceWriter& writer,
                 const lang::gc::Heap& heap,
                 std::uint64_t verify_index,
                 std::string_view check,
                 lang::TraceVerifyBoundary boundary) {
    writer.on_verify_step(
        heap, check, std::optional<std::uint64_t>{verify_index}, boundary);
}

void write_sampled_fixture(const std::filesystem::path& directory) {
    lang::gc::Heap heap;
    {
        lang::JsonlTraceWriter writer(
            directory, 8, lang::VerifyEventMode::Sampled);
        writer.on_program_start(heap);

        std::uint64_t verify_index = 0;
        for (std::uint64_t unscoped_index = 0;
             unscoped_index <= 40; ++unscoped_index) {
            emit_verify(writer, heap, verify_index++, "remembered_set",
                        lang::TraceVerifyBoundary::None);
        }

        writer.on_logical_collection_begin(
            heap, lang::TraceCollectionKind::Major);
        heap.collect();
        for (std::uint64_t collection_index = 0;
             collection_index < 35; ++collection_index) {
            const std::string_view check =
                collection_index == 1
                    ? "weak_targets"
                    : (collection_index == 5 ? "intern_table"
                                             : "remembered_set");
            const auto boundary =
                collection_index == 34
                    ? lang::TraceVerifyBoundary::CollectionEnd
                    : lang::TraceVerifyBoundary::None;
            emit_verify(writer, heap, verify_index++, check, boundary);
        }
        writer.on_logical_collection_end(heap);

        for (std::uint64_t unscoped_index = 41;
             unscoped_index <= 64; ++unscoped_index) {
            emit_verify(writer, heap, verify_index++, "remembered_set",
                        lang::TraceVerifyBoundary::None);
        }
        require(verify_index == 100,
                "fixture did not emit exactly 100 true verify callbacks");
        writer.on_program_exit(heap, 7);
    }
}

void write_default_full_fixture(const std::filesystem::path& directory) {
    lang::gc::Heap heap;
    {
        lang::JsonlTraceWriter writer(directory, 8);
        writer.on_program_start(heap);
        writer.on_verify_step(heap, "remembered_set",
                              std::optional<std::uint64_t>{0});

        writer.on_logical_collection_begin(
            heap, lang::TraceCollectionKind::Major);
        heap.collect();
        writer.on_verify_step(heap, "weak_targets",
                              std::optional<std::uint64_t>{1});
        writer.on_verify_step(heap, "remembered_set",
                              std::optional<std::uint64_t>{2});
        writer.on_verify_step(heap, "intern_table",
                              std::optional<std::uint64_t>{3});
        writer.on_logical_collection_end(heap);
        writer.on_program_exit(heap, 0);
    }
}

void verify_invalid_mode_is_rejected(
    const std::filesystem::path& directory) {
    bool rejected = false;
    try {
        lang::JsonlTraceWriter writer(
            directory, 8, static_cast<lang::VerifyEventMode>(255));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "writer accepted an unknown verify event mode");
}

void verify_sampled_events(const std::vector<std::string>& events) {
    require(events.size() == 50,
            "sampled fixture did not emit exactly 50 events");
    for (std::size_t index = 0; index < events.size(); ++index) {
        require(integer_field(events[index], "seq") == index,
                "emitted event seq values are not contiguous");
    }

    std::vector<std::string_view> verify_events;
    const std::string* collection_end = nullptr;
    for (const auto& event : events) {
        if (event.find("\"kind\":\"verify_step\"") !=
            std::string::npos) {
            verify_events.push_back(event);
        }
        if (event.find("\"op\":\"collection_end\"") !=
            std::string::npos) {
            collection_end = &event;
        }
    }
    require(verify_events.size() == 48,
            "sampled fixture did not retain exactly 48 verify events");
    require(collection_end != nullptr,
            "sampled fixture omitted collection_end evidence");

    std::vector<std::uint64_t> expected_verify_indexes;
    for (std::uint64_t index = 0; index < 40; ++index) {
        expected_verify_indexes.push_back(index);
    }
    const std::vector<std::uint64_t> sampled_tail{
        41, 42, 46, 57, 73, 75, 83, 99};
    expected_verify_indexes.insert(expected_verify_indexes.end(),
                                   sampled_tail.begin(),
                                   sampled_tail.end());

    const std::vector<std::uint64_t> sampled_scope_indexes{
        0, 1, 5, 16, 32, 34, 48, 64};
    for (std::size_t index = 0; index < verify_events.size(); ++index) {
        const auto& event = verify_events[index];
        const auto true_index = integer_field(event, "verify_index");
        require(true_index == expected_verify_indexes[index],
                "sampled verify indexes did not match the retention union");
        require(integer_field(event, "size") == true_index,
                "sampled verify event lost its fixture sentinel");

        const auto expected_scope_index =
            index < 40 ? static_cast<std::uint64_t>(index)
                       : sampled_scope_indexes[index - 40];
        require(integer_field(event, "scope_index") ==
                    expected_scope_index,
                "sampled verify scope indexes were not zero-based");

        const bool collection_scoped =
            true_index >= 41 && true_index <= 75;
        if (collection_scoped) {
            require(integer_field(event, "collection_id") == 0,
                    "collection verify event has the wrong collection id");
        } else {
            require_contains(
                event, "\"collection_id\":null",
                "unscoped verify event was assigned to a collection");
        }
        require(integer_field(event, "terminal") ==
                    (true_index == 75 ? 1U : 0U),
                "sampled verify terminal marker is incorrect");
    }

    for (std::size_t index = 0; index < 40; ++index) {
        require(integer_field(verify_events[index], "check_index") == index,
                "head verify check indexes were not zero-based");
        require(string_field(verify_events[index], "check") ==
                    "remembered_set",
                "head verify checks were not retained intact");
    }

    require(string_field(verify_events[40], "check") ==
                "remembered_set" &&
                integer_field(verify_events[40], "check_index") == 0,
            "collection-first remembered-set check was not retained");
    require(string_field(verify_events[41], "check") ==
                "weak_targets" &&
                integer_field(verify_events[41], "check_index") == 0,
            "first distinct weak-targets check was not retained");
    require(string_field(verify_events[42], "check") ==
                "intern_table" &&
                integer_field(verify_events[42], "check_index") == 0,
            "first distinct intern-table check was not retained");
    require(integer_field(verify_events[43], "scope_index") == 16 &&
                integer_field(verify_events[44], "scope_index") == 32,
            "collection verify stride did not use zero-based multiples of 16");
    require(integer_field(verify_events[45], "scope_index") == 34 &&
                integer_field(verify_events[45], "terminal") == 1,
            "terminal collection verify was not retained");
    require(integer_field(verify_events[46], "scope_index") == 48 &&
                integer_field(verify_events[47], "scope_index") == 64,
            "unscoped verify stride did not use zero-based multiples of 16");

    require_contains(*collection_end, "\"verify_true_count\":35",
                     "collection_end omitted its true verify count");
    require_contains(*collection_end, "\"verify_emitted_count\":6",
                     "collection_end omitted its emitted verify count");
    require_contains(
        *collection_end,
        "\"verify_checks\":[\"remembered_set\",\"weak_targets\",\"intern_table\"]",
        "collection_end omitted its deterministic distinct-check list");
}

void verify_snapshots(const std::vector<std::string>& snapshots,
                      std::size_t emitted_event_count) {
    require(!snapshots.empty(), "sampled fixture emitted no snapshots");
    require(integer_field(snapshots.front(), "seq") == 0,
            "initial sampled snapshot did not use seq zero");
    require(integer_field(snapshots.back(), "seq") ==
                emitted_event_count,
            "terminal sampled snapshot did not use the emitted event count");
    std::uint64_t prior = 0;
    for (const auto& snapshot : snapshots) {
        const auto seq = integer_field(snapshot, "seq");
        require(seq >= prior && seq <= emitted_event_count,
                "snapshot seq escaped the contiguous emitted sequence");
        prior = seq;
    }
}

void verify_sampled_stats(std::string_view stats) {
    require_contains(stats, "\"verify_events\":{",
                     "stats omitted the verify_events descriptor");
    require(string_field(stats, "mode") == "sampled",
            "stats reported the wrong verify event mode");
    require(integer_field(stats, "head_window") == 40,
            "stats reported the wrong verify head window");
    require(integer_field(stats, "stride") == 16,
            "stats reported the wrong verify stride");
    require(!string_field(stats, "retention_rule").empty(),
            "stats reported an empty verify retention rule");
    require(integer_field(stats, "true_count") == 100,
            "stats reported the wrong true verify count");
    require(integer_field(stats, "emitted_count") == 48,
            "stats reported the wrong emitted verify count");
    require(integer_field(stats, "unscoped_true_count") == 65,
            "stats reported the wrong unscoped true verify count");
    require(integer_field(stats, "unscoped_emitted_count") == 42,
            "stats reported the wrong unscoped emitted verify count");
    require(integer_field(stats, "verify_step") == 48,
            "event_totals.verify_step did not remain the emitted count");
}

void verify_default_full_bundle(
    const std::vector<std::string>& events,
    const std::vector<std::string>& snapshots,
    std::string_view stats) {
    const std::vector<std::string_view> expected_checks{
        "remembered_set", "weak_targets", "remembered_set", "intern_table"};
    require(events.size() == expected_checks.size() + 2,
            "default full mode emitted the wrong event count");
    for (std::size_t index = 0; index < events.size(); ++index) {
        require(integer_field(events[index], "seq") == index,
                "default full mode seq values are not contiguous");
    }

    std::vector<std::string_view> verify_events;
    const std::string* collection_end = nullptr;
    for (const auto& event : events) {
        if (event.find("\"kind\":\"verify_step\"") !=
            std::string::npos) {
            verify_events.push_back(event);
        }
        if (event.find("\"op\":\"collection_end\"") !=
            std::string::npos) {
            collection_end = &event;
        }
    }
    require(verify_events.size() == expected_checks.size(),
            "default full mode did not emit every verify callback");
    require(collection_end != nullptr,
            "default full mode omitted its collection end");
    require(collection_end->find("\"verify_true_count\":") ==
                std::string::npos &&
                collection_end->find("\"verify_emitted_count\":") ==
                std::string::npos &&
                collection_end->find("\"verify_checks\":") ==
                std::string::npos,
            "default full collection end exposed sampled-only metadata");

    for (std::size_t index = 0; index < verify_events.size(); ++index) {
        const auto& event = verify_events[index];
        const auto legacy_suffix =
            ",\"check\":\"" + std::string(expected_checks[index]) + "\"}";
        require(event.ends_with(legacy_suffix),
                "default full verify event changed its legacy shape");
        require(event.find("\"verify_index\":") ==
                    std::string::npos &&
                    event.find("\"collection_id\":") ==
                    std::string::npos &&
                    event.find("\"scope_index\":") ==
                    std::string::npos &&
                    event.find("\"check_index\":") ==
                    std::string::npos &&
                    event.find("\"terminal\":") ==
                    std::string::npos,
                "default full verify event exposed sampled-only metadata");
    }
    verify_snapshots(snapshots, events.size());

    require(string_field(stats, "mode") == "full",
            "default writer did not report full verify mode");
    require(integer_field(stats, "true_count") == 4,
            "full stats reported the wrong true verify count");
    require(integer_field(stats, "emitted_count") == 4,
            "full stats reported the wrong emitted verify count");
    require(integer_field(stats, "unscoped_true_count") == 1,
            "full stats reported the wrong unscoped true count");
    require(integer_field(stats, "unscoped_emitted_count") == 1,
            "full stats reported the wrong unscoped emitted count");
    require(integer_field(stats, "verify_step") == 4,
            "full event_totals.verify_step did not equal emitted count");
}

} // namespace

int main() {
    try {
        const auto root =
            std::filesystem::current_path() /
            "trace_verify_sampling_artifacts";
        const auto sampled_directory = root / "sampled";
        write_sampled_fixture(sampled_directory);

        const auto events =
            read_lines(sampled_directory / "events.jsonl");
        const auto snapshots =
            read_lines(sampled_directory / "snapshots.jsonl");
        const auto stats = read_file(sampled_directory / "stats.json");
        verify_sampled_events(events);
        verify_snapshots(snapshots, events.size());
        verify_sampled_stats(stats);

        const auto full_directory = root / "full";
        write_default_full_fixture(full_directory);
        verify_default_full_bundle(
            read_lines(full_directory / "events.jsonl"),
            read_lines(full_directory / "snapshots.jsonl"),
            read_file(full_directory / "stats.json"));
        verify_invalid_mode_is_rejected(root / "invalid-mode");

        std::cerr << "[PASS] sampled and default-full verify contracts\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
