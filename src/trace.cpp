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

std::string_view trace_collection_kind_label(TraceCollectionKind kind) {
    switch (kind) {
    case TraceCollectionKind::Major:
        return "major";
    case TraceCollectionKind::Minor:
        return "minor";
    }
    throw std::logic_error("unknown trace collection kind");
}

std::string_view trace_move_cause_label(TraceMoveCause cause) {
    switch (cause) {
    case TraceMoveCause::AtomicMajor:
        return "atomic_major";
    case TraceMoveCause::AtomicMinor:
        return "atomic_minor";
    case TraceMoveCause::IncrementalDeathAccounting:
        return "incremental_death_accounting";
    case TraceMoveCause::IncrementalCompactionStep:
        return "incremental_compaction_step";
    case TraceMoveCause::IncrementalCompactionFinalize:
        return "incremental_compaction_finalize";
    case TraceMoveCause::IncrementalMarkCompact:
        return "incremental_mark_compact";
    case TraceMoveCause::MapGrowth:
        return "map_growth";
    case TraceMoveCause::BuilderGrowth:
        return "builder_growth";
    }
    throw std::logic_error("unknown trace move cause");
}

std::string_view trace_move_kind_label(TraceMoveKind kind) {
    switch (kind) {
    case TraceMoveKind::Compaction:
        return "compaction";
    case TraceMoveKind::Growth:
        return "growth";
    }
    throw std::logic_error("unknown trace move kind");
}

std::string_view trace_forward_kind_label(TraceForwardKind kind) {
    switch (kind) {
    case TraceForwardKind::Heap:
        return "heap";
    case TraceForwardKind::Root:
        return "root";
    case TraceForwardKind::Registry:
        return "registry";
    }
    throw std::logic_error("unknown trace forward kind");
}

std::size_t trace_forward_kind_index(TraceForwardKind kind) {
    switch (kind) {
    case TraceForwardKind::Heap:
        return 0;
    case TraceForwardKind::Root:
        return 1;
    case TraceForwardKind::Registry:
        return 2;
    }
    throw std::logic_error("unknown trace forward kind");
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
    struct ActiveCollection {
        std::uint64_t id{0};
        TraceCollectionKind kind{TraceCollectionKind::Major};
    };

    struct ActiveMove {
        std::uint64_t id{0};
        TraceMoveCause cause{TraceMoveCause::AtomicMajor};
    };

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
    void write_event_raw(std::string_view kind, const EventFields& fields,
                         AdditiveWriter&& write_additive) {
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

    void reconcile_heap(const gc::Heap& heap, bool mutator_side) {
        const auto physical_objects = heap.trace_snapshot();
        peak_live_bytes = std::max(
            peak_live_bytes, live_bytes(physical_objects));
        if (physical_objects.size() != logical_objects.size()) {
            throw std::logic_error(
                "trace mirror object count changed without a lifecycle event");
        }
        for (std::size_t index = 0; index < physical_objects.size(); ++index) {
            const auto& physical = physical_objects[index];
            auto& logical = logical_objects[index];
            if (physical.id != logical.id || physical.kind != logical.kind ||
                physical.generation != logical.generation) {
                throw std::logic_error(
                    "trace mirror identity changed without a lifecycle event");
            }
            if (physical.size == logical.size &&
                physical.references == logical.references) {
                continue;
            }
            EventFields fields;
            fields.id = physical.id;
            fields.size = physical.size;
            fields.generation = physical.generation;
            fields.references = std::span<const ObjectId>(
                physical.references.data(), physical.references.size());
            fields.mutator_side = mutator_side;
            write_event_raw(
                "update", fields,
                [&physical](std::ostream& output) {
                    output << ",\"object_kind\":";
                    write_json_string(output, physical.kind);
                });
            logical = physical;
        }
    }

    template <typename AdditiveWriter>
    void write_event(const gc::Heap& heap, std::string_view kind,
                     const EventFields& fields,
                     AdditiveWriter&& write_additive) {
        if (move_transactions.empty()) {
            reconcile_heap(heap, false);
        }
        write_event_raw(kind, fields,
                        std::forward<AdditiveWriter>(write_additive));
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
        const auto lifetime_collection_count =
            metrics.major_collections + metrics.minor_collections;
        if (lifetime_collection_count < collection_count_baseline) {
            throw std::logic_error(
                "trace heap collection metrics moved backwards");
        }
        const auto collection_count =
            lifetime_collection_count - collection_count_baseline;
        if (active_collection.has_value() || !move_transactions.empty()) {
            throw std::logic_error(
                "trace run ended inside a collection transaction");
        }
        if (collection_count != next_collection_id) {
            throw std::logic_error(
                "trace logical collection evidence disagrees with metrics");
        }

        std::ofstream stats(output_directory / "stats.json",
                            std::ios::binary | std::ios::trunc);
        if (!stats) {
            throw std::runtime_error("could not open trace stats.json");
        }
        stats << "{\"live_bytes_final\":" << final_live_bytes
              << ",\"forwarded_reference_count\":"
              << forwarded_reference_count
              << ",\"forwarded_reference_totals\":{\"heap\":"
              << forwarded_reference_totals[0]
              << ",\"root\":" << forwarded_reference_totals[1]
              << ",\"registry\":" << forwarded_reference_totals[2]
              << "}"
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
        stats << "},\"snapshot_interval\":" << snapshot_interval
              << ",\"ticks\":" << retired_ticks
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
    std::array<std::uint64_t, 3> forwarded_reference_totals{};
    std::uint64_t pause_slices{0};
    std::uint64_t peak_live_bytes{0};
    bool started{false};
    bool finished{false};
    std::optional<ActiveCollection> active_collection;
    std::vector<ActiveMove> move_transactions;
    std::uint64_t next_collection_id{0};
    std::uint64_t next_transaction_id{0};
    std::uint64_t collection_count_baseline{0};
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
    const auto metrics = heap.metrics();
    impl_->collection_count_baseline =
        metrics.major_collections + metrics.minor_collections;
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
    impl_->reconcile_heap(heap, false);
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
    std::uint64_t to_slot, ObjectId destination_id,
    TraceMoveKind move_kind) {
    EventFields fields;
    fields.id = source_id;
    fields.size = size;
    fields.generation = source_generation;
    fields.from = from_slot;
    fields.to = to_slot;
    impl_->write_event(heap, "relocate", fields,
                       [destination_id, move_kind](std::ostream& output) {
                           output << ",\"to_id\":" << destination_id
                                  << ",\"move_kind\":";
                           write_json_string(
                               output, trace_move_kind_label(move_kind));
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

void JsonlTraceWriter::on_reference_forwarded(
    ObjectId source_id, ObjectId destination_id,
    TraceForwardKind kind, std::optional<ObjectId> owner_id) {
    if (source_id == destination_id) {
        throw std::logic_error(
            "trace forwarding evidence requires unequal object IDs");
    }
    if ((kind == TraceForwardKind::Heap) != owner_id.has_value()) {
        throw std::logic_error(
            "trace heap forwarding evidence requires exactly one owner ID");
    }
    EventFields fields;
    const auto collection_id =
        impl_->active_collection.has_value()
            ? std::optional<std::uint64_t>(impl_->active_collection->id)
            : std::nullopt;
    impl_->write_event_raw(
        "gc", fields,
        [collection_id, source_id, destination_id, owner_id,
         kind](std::ostream& output) {
            output << ",\"op\":\"forward\",\"collection_id\":";
            if (collection_id.has_value()) {
                output << *collection_id;
            } else {
                output << "null";
            }
            output << ",\"from_id\":" << source_id;
            output << ",\"to_id\":" << destination_id;
            output << ",\"owner_id\":";
            if (owner_id.has_value()) {
                output << *owner_id;
            } else {
                output << "null";
            }
            output << ",\"forward_kind\":";
            write_json_string(output, trace_forward_kind_label(kind));
        });
    ++impl_->forwarded_reference_count;
    ++impl_->forwarded_reference_totals[
        trace_forward_kind_index(kind)];
}

void JsonlTraceWriter::on_pause_slice() {
    EventFields fields;
    const auto collection_id =
        impl_->active_collection.has_value()
            ? std::optional<std::uint64_t>(impl_->active_collection->id)
            : std::nullopt;
    impl_->write_event_raw(
        "gc", fields,
        [collection_id](std::ostream& output) {
            output << ",\"op\":\"pause\",\"collection_id\":";
            if (collection_id.has_value()) {
                output << *collection_id;
            } else {
                output << "null";
            }
        });
    ++impl_->pause_slices;
}

void JsonlTraceWriter::on_heap_sample(const gc::Heap& heap) {
    impl_->reconcile_heap(heap, true);
}

void JsonlTraceWriter::on_logical_collection_begin(
    const gc::Heap& heap, TraceCollectionKind kind) {
    if (impl_->active_collection.has_value()) {
        throw std::logic_error(
            "trace logical collection began while another was active");
    }
    impl_->reconcile_heap(heap, false);
    const auto collection_id = impl_->next_collection_id;
    const auto kind_label = trace_collection_kind_label(kind);
    const auto bytes = live_bytes(impl_->logical_objects);
    const auto objects = impl_->logical_objects.size();
    EventFields fields;
    impl_->write_event_raw(
        "gc", fields,
        [collection_id, kind_label, bytes, objects](std::ostream& output) {
            output << ",\"op\":\"collection_begin\",\"collection_id\":"
                   << collection_id << ",\"collection_kind\":";
            write_json_string(output, kind_label);
            output << ",\"live_bytes\":" << bytes
                   << ",\"live_objects\":" << objects;
        });
    impl_->active_collection =
        Impl::ActiveCollection{collection_id, kind};
    ++impl_->next_collection_id;
}

void JsonlTraceWriter::on_logical_collection_end(
    const gc::Heap& heap) {
    if (!impl_->active_collection.has_value()) {
        throw std::logic_error(
            "trace logical collection ended without a begin");
    }
    if (!impl_->move_transactions.empty()) {
        throw std::logic_error(
            "trace logical collection ended inside a move transaction");
    }
    impl_->reconcile_heap(heap, false);
    const auto collection = *impl_->active_collection;
    const auto kind_label = trace_collection_kind_label(collection.kind);
    const auto bytes = live_bytes(impl_->logical_objects);
    const auto objects = impl_->logical_objects.size();
    EventFields fields;
    impl_->write_event_raw(
        "gc", fields,
        [collection, kind_label, bytes, objects](std::ostream& output) {
            output << ",\"op\":\"collection_end\",\"collection_id\":"
                   << collection.id << ",\"collection_kind\":";
            write_json_string(output, kind_label);
            output << ",\"live_bytes\":" << bytes
                   << ",\"live_objects\":" << objects;
        });
    impl_->active_collection.reset();
}

void JsonlTraceWriter::on_move_begin(const gc::Heap& heap,
                                     TraceMoveCause cause) {
    impl_->reconcile_heap(heap, false);
    const auto transaction_id = impl_->next_transaction_id;
    const auto parent_transaction_id =
        impl_->move_transactions.empty()
            ? std::optional<std::uint64_t>{}
            : std::optional<std::uint64_t>{
                  impl_->move_transactions.back().id};
    const auto depth = impl_->move_transactions.size() + 1;
    const auto cause_label = trace_move_cause_label(cause);
    const auto collection_id =
        impl_->active_collection.has_value()
            ? std::optional<std::uint64_t>{impl_->active_collection->id}
            : std::optional<std::uint64_t>{};
    const auto bytes = live_bytes(impl_->logical_objects);
    const auto objects = impl_->logical_objects.size();
    EventFields fields;
    impl_->write_event_raw(
        "gc", fields,
        [transaction_id, parent_transaction_id, depth, cause_label,
         collection_id, bytes, objects](std::ostream& output) {
            output << ",\"op\":\"move_begin\",\"transaction_id\":"
                   << transaction_id << ",\"parent_transaction_id\":";
            if (parent_transaction_id.has_value()) {
                output << *parent_transaction_id;
            } else {
                output << "null";
            }
            output << ",\"depth\":" << depth << ",\"cause\":";
            write_json_string(output, cause_label);
            output << ",\"collection_id\":";
            if (collection_id.has_value()) {
                output << *collection_id;
            } else {
                output << "null";
            }
            output << ",\"live_bytes\":" << bytes
                   << ",\"live_objects\":" << objects;
        });
    impl_->move_transactions.push_back(
        Impl::ActiveMove{transaction_id, cause});
    ++impl_->next_transaction_id;
}

void JsonlTraceWriter::on_move_end(const gc::Heap& heap) {
    if (impl_->move_transactions.empty()) {
        throw std::logic_error(
            "trace move transaction ended without a begin");
    }
    impl_->reconcile_heap(heap, false);
    const auto transaction = impl_->move_transactions.back();
    const auto parent_transaction_id =
        impl_->move_transactions.size() > 1
            ? std::optional<std::uint64_t>{
                  impl_->move_transactions[
                      impl_->move_transactions.size() - 2].id}
            : std::optional<std::uint64_t>{};
    const auto depth = impl_->move_transactions.size();
    const auto cause_label = trace_move_cause_label(transaction.cause);
    const auto collection_id =
        impl_->active_collection.has_value()
            ? std::optional<std::uint64_t>{impl_->active_collection->id}
            : std::optional<std::uint64_t>{};
    const auto bytes = live_bytes(impl_->logical_objects);
    const auto objects = impl_->logical_objects.size();
    EventFields fields;
    impl_->write_event_raw(
        "gc", fields,
        [transaction, parent_transaction_id, depth, cause_label,
         collection_id, bytes, objects](std::ostream& output) {
            output << ",\"op\":\"move_end\",\"transaction_id\":"
                   << transaction.id
                   << ",\"parent_transaction_id\":";
            if (parent_transaction_id.has_value()) {
                output << *parent_transaction_id;
            } else {
                output << "null";
            }
            output << ",\"depth\":" << depth << ",\"cause\":";
            write_json_string(output, cause_label);
            output << ",\"collection_id\":";
            if (collection_id.has_value()) {
                output << *collection_id;
            } else {
                output << "null";
            }
            output << ",\"live_bytes\":" << bytes
                   << ",\"live_objects\":" << objects;
        });
    impl_->move_transactions.pop_back();
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
