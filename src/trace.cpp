#include "lang/trace.hpp"

#include "lang/gc/heap.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace lang {

namespace {

constexpr std::array<std::string_view, 15> kScheduleNames{
    "no_stress",
    "before_every_alloc",
    "after_every_alloc",
    "major_every_1",
    "major_every_3",
    "major_every_7",
    "minor_every_1",
    "minor_every_4",
    "minor_after_every_barrier",
    "incremental_1",
    "incremental_3_1",
    "combined",
    "incremental_compact_1",
    "incremental_compact_3_1",
    "combined_mark_compact",
};

constexpr std::array<std::string_view, 9> kRequiredEventKinds{
    "alloc", "mark_slice", "relocate", "promote", "die",
    "intern", "evict", "trap", "verify_step",
};

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
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
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

std::uint64_t live_bytes(
    const std::vector<TraceHeapObject>& objects) {
    std::uint64_t slots = 0;
    for (const auto& object : objects) {
        if (object.size >
            std::numeric_limits<std::uint64_t>::max() - slots) {
            throw std::overflow_error("trace live-slot total overflow");
        }
        slots += object.size;
    }
    if (slots > std::numeric_limits<std::uint64_t>::max() / 8) {
        throw std::overflow_error("trace live-byte total overflow");
    }
    return slots * 8;
}

std::uint32_t trace_slot(ObjectId id) {
    return static_cast<std::uint32_t>(id & 0xffff'ffffull);
}

struct EventFields {
    std::optional<ObjectId> id;
    std::optional<std::uint64_t> size;
    std::optional<std::uint8_t> generation;
    std::optional<std::uint64_t> from;
    std::optional<std::uint64_t> to;
    std::optional<std::span<const ObjectId>> references;
    bool mutator_side{false};
};

} // namespace

struct JsonlTraceWriter::Impl {
    explicit Impl(std::filesystem::path directory, std::size_t interval)
        : output_directory(std::move(directory)), snapshot_interval(interval) {
        if (snapshot_interval == 0) {
            throw std::invalid_argument(
                "trace snapshot interval must be positive");
        }
        std::filesystem::create_directories(output_directory);
        events.open(output_directory / "events.jsonl",
                    std::ios::binary | std::ios::trunc);
        snapshots.open(output_directory / "snapshots.jsonl",
                       std::ios::binary | std::ios::trunc);
        if (!events || !snapshots) {
            throw std::runtime_error(
                "could not open trace JSONL output files");
        }
    }

    void ensure_writable() {
        if (!events || !snapshots) {
            throw std::runtime_error("failed to write trace JSONL output");
        }
    }

    void synchronize_heap(const gc::Heap& heap) {
        logical_objects = heap.trace_snapshot();
        peak_live_bytes = std::max(
            peak_live_bytes, live_bytes(logical_objects));
    }

    void sample_heap(const gc::Heap& heap) {
        const auto objects = heap.trace_snapshot();
        peak_live_bytes = std::max(
            peak_live_bytes, live_bytes(objects));
        if (collection_depth == 0) {
            logical_objects = objects;
        }
    }

    void write_snapshot() {
        peak_live_bytes = std::max(
            peak_live_bytes, live_bytes(logical_objects));
        snapshots << "{\"tick\":" << tick << ",\"seq\":" << seq
                  << ",\"live\":[";
        for (std::size_t i = 0; i < logical_objects.size(); ++i) {
            if (i != 0) {
                snapshots.put(',');
            }
            const auto& object = logical_objects[i];
            snapshots << "{\"id\":" << object.id << ",\"kind\":";
            write_json_string(snapshots, object.kind);
            snapshots << ",\"size\":" << object.size
                      << ",\"gen\":"
                      << static_cast<std::uint64_t>(object.generation)
                      << ",\"refs\":[";
            for (std::size_t ref = 0;
                 ref < object.references.size(); ++ref) {
                if (ref != 0) {
                    snapshots.put(',');
                }
                snapshots << object.references[ref];
            }
            snapshots << "]}";
        }
        snapshots << "]}\n";
        ensure_writable();
    }

    template <typename AdditiveWriter>
    void write_event(const gc::Heap& heap, std::string_view kind,
                     const EventFields& fields,
                     AdditiveWriter&& write_additive) {
        if (collection_depth == 0) {
            synchronize_heap(heap);
        }
        if (seq != 0 && seq % snapshot_interval == 0 &&
            last_periodic_snapshot_seq != seq) {
            write_snapshot();
            last_periodic_snapshot_seq = seq;
        }

        events << "{\"tick\":" << tick << ",\"seq\":" << seq
               << ",\"kind\":";
        write_json_string(events, kind);
        events << ",\"id\":";
        if (fields.id.has_value()) {
            events << *fields.id;
        } else {
            events << "null";
        }
        events << ",\"size\":";
        if (fields.size.has_value()) {
            events << *fields.size;
        } else {
            events << "null";
        }
        events << ",\"gen\":";
        if (fields.generation.has_value()) {
            events << static_cast<std::uint64_t>(*fields.generation);
        } else {
            events << "null";
        }
        events << ",\"from\":";
        if (fields.from.has_value()) {
            events << *fields.from;
        } else {
            events << "null";
        }
        events << ",\"to\":";
        if (fields.to.has_value()) {
            events << *fields.to;
        } else {
            events << "null";
        }
        events << ",\"refs\":";
        if (fields.references.has_value()) {
            events.put('[');
            const auto refs = *fields.references;
            for (std::size_t i = 0; i < refs.size(); ++i) {
                if (i != 0) {
                    events.put(',');
                }
                events << refs[i];
            }
            events.put(']');
        } else {
            events << "null";
        }
        events << ",\"src_pos\":";
        if (fields.mutator_side && source_position.has_value()) {
            events << "{\"line\":" << source_position->line
                   << ",\"col\":" << source_position->column;
            if (source_position->function.has_value()) {
                events << ",\"fn\":";
                write_json_string(events,
                                  *source_position->function);
            }
            events.put('}');
        } else {
            events << "null";
        }
        write_additive(events);
        events << "}\n";
        ++event_totals[std::string(kind)];
        ++seq;
        ensure_writable();
    }

    void add_object(ObjectId id, std::string_view kind,
                    std::uint64_t size, std::uint8_t generation,
                    std::span<const ObjectId> references) {
        TraceHeapObject object;
        object.id = id;
        object.kind = kind;
        object.size = size;
        object.generation = generation;
        object.references.assign(references.begin(), references.end());
        logical_objects.push_back(std::move(object));
        sort_logical_objects();
    }

    void remove_object(ObjectId id) {
        logical_objects.erase(
            std::remove_if(
                logical_objects.begin(), logical_objects.end(),
                [id](const TraceHeapObject& object) {
                    return object.id == id;
                }),
            logical_objects.end());
        for (auto& object : logical_objects) {
            object.references.erase(
                std::remove(object.references.begin(),
                            object.references.end(), id),
                object.references.end());
        }
    }

    void relocate_object(ObjectId source, ObjectId destination) {
        for (auto& object : logical_objects) {
            if (object.id == source) {
                object.id = destination;
            }
            for (auto& reference : object.references) {
                if (reference == source) {
                    reference = destination;
                }
            }
        }
        sort_logical_objects();
    }

    void promote_object(ObjectId source, ObjectId destination) {
        relocate_object(source, destination);
        for (auto& object : logical_objects) {
            if (object.id == destination) {
                object.generation = 1;
                return;
            }
        }
    }

    void sort_logical_objects() {
        std::sort(logical_objects.begin(), logical_objects.end(),
                  [](const TraceHeapObject& left,
                     const TraceHeapObject& right) {
                      return trace_slot(left.id) < trace_slot(right.id);
                  });
    }

    void write_stats(const gc::Heap& heap, std::uint64_t retired_ticks) {
        const auto objects = heap.trace_snapshot();
        const auto final_live_bytes = live_bytes(objects);
        peak_live_bytes = std::max(peak_live_bytes, final_live_bytes);
        const auto metrics = heap.metrics();
        const auto collection_count =
            metrics.major_collections + metrics.minor_collections;

        std::ofstream stats(output_directory / "stats.json",
                            std::ios::binary | std::ios::trunc);
        if (!stats) {
            throw std::runtime_error("could not open trace stats.json");
        }
        stats << "{\"live_bytes_final\":" << final_live_bytes
              << ",\"forwarded_reference_count\":"
              << forwarded_reference_count
              << ",\"pause_slices\":" << pause_slices
              << ",\"collection_count\":" << collection_count
              << ",\"event_totals\":{";
        for (std::size_t i = 0; i < kRequiredEventKinds.size(); ++i) {
            if (i != 0) {
                stats.put(',');
            }
            write_json_string(stats, kRequiredEventKinds[i]);
            stats.put(':');
            const auto found =
                event_totals.find(std::string(kRequiredEventKinds[i]));
            stats << (found == event_totals.end() ? 0 : found->second);
        }
        stats << "},\"ticks\":" << retired_ticks
              << ",\"peak_live_bytes\":" << peak_live_bytes << "}\n";
        if (!stats) {
            throw std::runtime_error("failed to write trace stats.json");
        }
    }

    std::filesystem::path output_directory;
    std::size_t snapshot_interval{4096};
    std::ofstream events;
    std::ofstream snapshots;
    std::uint64_t tick{0};
    std::uint64_t seq{0};
    std::uint64_t last_periodic_snapshot_seq{
        std::numeric_limits<std::uint64_t>::max()};
    std::optional<TraceSourcePosition> source_position;
    std::vector<TraceHeapObject> logical_objects;
    std::map<std::string, std::uint64_t> event_totals;
    std::uint64_t forwarded_reference_count{0};
    std::uint64_t pause_slices{0};
    std::uint64_t peak_live_bytes{0};
    bool started{false};
    bool finished{false};
    std::size_t collection_depth{0};
};

JsonlTraceWriter::JsonlTraceWriter(
    std::filesystem::path output_directory,
    std::size_t snapshot_interval)
    : impl_(std::make_unique<Impl>(std::move(output_directory),
                                   snapshot_interval)) {}

JsonlTraceWriter::~JsonlTraceWriter() = default;
JsonlTraceWriter::JsonlTraceWriter(JsonlTraceWriter&&) noexcept = default;
JsonlTraceWriter&
JsonlTraceWriter::operator=(JsonlTraceWriter&&) noexcept = default;

void JsonlTraceWriter::set_mutator_context(
    std::uint64_t tick,
    std::optional<TraceSourcePosition> source_position) {
    impl_->tick = tick;
    impl_->source_position = std::move(source_position);
}

void JsonlTraceWriter::on_program_start(const gc::Heap& heap) {
    if (impl_->started) {
        throw std::logic_error("trace writer cannot start more than one run");
    }
    impl_->started = true;
    impl_->tick = 0;
    impl_->source_position.reset();
    impl_->synchronize_heap(heap);
    impl_->write_snapshot();
}

void JsonlTraceWriter::on_program_exit(const gc::Heap& heap,
                                       std::uint64_t retired_ticks) {
    if (!impl_->started || impl_->finished) {
        throw std::logic_error("trace writer received an invalid run exit");
    }
    impl_->tick = retired_ticks;
    impl_->source_position.reset();
    impl_->synchronize_heap(heap);
    impl_->write_snapshot();
    impl_->write_stats(heap, retired_ticks);
    impl_->finished = true;
}

void JsonlTraceWriter::on_alloc(
    const gc::Heap& heap, ObjectId id, std::uint64_t size,
    std::uint8_t generation, std::span<const ObjectId> references,
    std::string_view object_kind) {
    EventFields fields;
    fields.id = id;
    fields.size = size;
    fields.generation = generation;
    fields.references = references;
    fields.mutator_side = true;
    impl_->write_event(heap, "alloc", fields,
                       [object_kind](std::ostream& output) {
                           output << ",\"object_kind\":";
                           write_json_string(output, object_kind);
                       });
    impl_->add_object(id, object_kind, size, generation, references);
}

void JsonlTraceWriter::on_mark_slice(const gc::Heap& heap,
                                     std::uint64_t objects_scanned) {
    EventFields fields;
    fields.size = objects_scanned;
    impl_->write_event(heap, "mark_slice", fields,
                       [](std::ostream&) {});
}

void JsonlTraceWriter::on_relocate(
    const gc::Heap& heap, ObjectId source_id, std::uint64_t size,
    std::uint8_t source_generation, std::uint64_t from_slot,
    std::uint64_t to_slot, ObjectId destination_id) {
    EventFields fields;
    fields.id = source_id;
    fields.size = size;
    fields.generation = source_generation;
    fields.from = from_slot;
    fields.to = to_slot;
    impl_->write_event(heap, "relocate", fields,
                       [destination_id](std::ostream& output) {
                           output << ",\"to_id\":" << destination_id;
                       });
    impl_->relocate_object(source_id, destination_id);
}

void JsonlTraceWriter::on_promote(
    const gc::Heap& heap, ObjectId source_id, std::uint64_t size,
    std::uint64_t from_slot, std::uint64_t to_slot,
    ObjectId destination_id) {
    EventFields fields;
    fields.id = source_id;
    fields.size = size;
    fields.generation = 0;
    fields.from = from_slot;
    fields.to = to_slot;
    impl_->write_event(heap, "promote", fields,
                       [destination_id](std::ostream& output) {
                           output << ",\"to_id\":" << destination_id;
                       });
    impl_->promote_object(source_id, destination_id);
}

void JsonlTraceWriter::on_die(const gc::Heap& heap, ObjectId id,
                              std::uint64_t size,
                              std::uint8_t generation) {
    EventFields fields;
    fields.id = id;
    fields.size = size;
    fields.generation = generation;
    impl_->write_event(heap, "die", fields, [](std::ostream&) {});
    impl_->remove_object(id);
}

void JsonlTraceWriter::on_intern(const gc::Heap& heap,
                                 ObjectId canonical,
                                 std::uint64_t size,
                                 std::uint8_t generation, bool hit) {
    EventFields fields;
    fields.id = canonical;
    fields.size = size;
    fields.generation = generation;
    fields.mutator_side = true;
    impl_->write_event(heap, "intern", fields,
                       [hit](std::ostream& output) {
                           output << ",\"hit\":" << (hit ? 1 : 0);
                       });
}

void JsonlTraceWriter::on_evict(const gc::Heap& heap,
                                ObjectId canonical,
                                std::uint64_t size,
                                std::uint8_t generation) {
    EventFields fields;
    fields.id = canonical;
    fields.size = size;
    fields.generation = generation;
    impl_->write_event(heap, "evict", fields, [](std::ostream&) {});
}

void JsonlTraceWriter::on_trap(const gc::Heap& heap,
                               std::string_view reason) {
    EventFields fields;
    fields.mutator_side = true;
    impl_->write_event(heap, "trap", fields,
                       [reason](std::ostream& output) {
                           output << ",\"reason\":";
                           write_json_string(output, reason);
                       });
}

void JsonlTraceWriter::on_verify_step(
    const gc::Heap& heap, std::string_view check,
    std::optional<std::uint64_t> elements_examined) {
    EventFields fields;
    fields.size = elements_examined;
    impl_->write_event(heap, "verify_step", fields,
                       [check](std::ostream& output) {
                           output << ",\"check\":";
                           write_json_string(output, check);
                       });
}

void JsonlTraceWriter::on_reference_forwarded() {
    ++impl_->forwarded_reference_count;
}

void JsonlTraceWriter::on_pause_slice() {
    ++impl_->pause_slices;
}

void JsonlTraceWriter::on_heap_sample(const gc::Heap& heap) {
    impl_->sample_heap(heap);
}

void JsonlTraceWriter::on_collection_begin(const gc::Heap& heap) {
    impl_->synchronize_heap(heap);
    ++impl_->collection_depth;
}

void JsonlTraceWriter::on_collection_end(const gc::Heap& heap) {
    if (impl_->collection_depth == 0) {
        throw std::logic_error(
            "trace collection transaction ended without a begin");
    }
    impl_->synchronize_heap(heap);
    --impl_->collection_depth;
}

std::span<const std::string_view> trace_schedule_names() {
    return kScheduleNames;
}

bool configure_trace_schedule(std::string_view name,
                              gc::StressConfig& config) {
    config = {};
    if (name == "no_stress") {
        return true;
    }
    if (name == "before_every_alloc") {
        config.collect_before_every_allocation = true;
        return true;
    }
    if (name == "after_every_alloc") {
        config.collect_after_every_allocation = true;
        return true;
    }
    if (name == "major_every_1" || name == "major_every_3" ||
        name == "major_every_7") {
        config.collect_every_n_instructions =
            name == "major_every_1" ? 1 :
            (name == "major_every_3" ? 3 : 7);
        return true;
    }
    if (name == "minor_every_1" || name == "minor_every_4") {
        config.collect_minor_every_n_instructions =
            name == "minor_every_1" ? 1 : 4;
        return true;
    }
    if (name == "minor_after_every_barrier") {
        config.collect_minor_after_every_write_barrier = true;
        return true;
    }
    if (name == "incremental_1") {
        config.incremental_mark_step_budgets = {1};
        return true;
    }
    if (name == "incremental_3_1") {
        config.incremental_mark_step_budgets = {3, 1};
        return true;
    }
    if (name == "combined" || name == "combined_mark_compact") {
        config.collect_before_every_allocation = true;
        config.collect_after_every_allocation = true;
        config.collect_every_n_instructions = 7;
        config.collect_minor_every_n_instructions = 4;
        config.collect_minor_after_every_write_barrier = true;
        config.incremental_mark_step_budgets = {2, 1};
        if (name == "combined_mark_compact") {
            config.incremental_compact_step_budgets = {1, 2};
        }
        return true;
    }
    if (name == "incremental_compact_1") {
        config.incremental_compact_step_budgets = {1};
        return true;
    }
    if (name == "incremental_compact_3_1") {
        config.incremental_compact_step_budgets = {3, 1};
        return true;
    }
    return false;
}

} // namespace lang
