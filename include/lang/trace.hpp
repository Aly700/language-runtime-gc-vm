#pragma once

#include "lang/value.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

namespace gc {
class Heap;
struct StressConfig;
} // namespace gc

struct TraceSourcePosition {
    std::uint64_t line{1};
    std::uint64_t column{1};
    std::optional<std::string> function;
};

struct TraceHeapObject {
    ObjectId id{0};
    std::string kind;
    std::uint64_t size{0};
    std::uint8_t generation{0};
    std::vector<ObjectId> references;
};

enum class TraceCollectionKind : std::uint8_t {
    Major,
    Minor,
};

enum class TraceMoveCause : std::uint8_t {
    AtomicMajor,
    AtomicMinor,
    IncrementalDeathAccounting,
    IncrementalCompactionStep,
    IncrementalCompactionFinalize,
    IncrementalMarkCompact,
    MapGrowth,
    BuilderGrowth,
};

enum class TraceMoveKind : std::uint8_t {
    Compaction,
    Growth,
};

enum class TraceForwardKind : std::uint8_t {
    Heap,
    Root,
    Registry,
};

// TraceSink is a one-way observer. Runtime code never reads a value back from a
// sink, and a null sink leaves every existing collector path untouched.
class TraceSink {
public:
    virtual ~TraceSink() = default;

    virtual void set_mutator_context(
        std::uint64_t tick,
        std::optional<TraceSourcePosition> source_position) = 0;
    virtual void on_program_start(const gc::Heap& heap) = 0;
    virtual void on_program_exit(const gc::Heap& heap,
                                 std::uint64_t retired_ticks) = 0;

    virtual void on_alloc(const gc::Heap& heap, ObjectId id,
                          std::uint64_t size, std::uint8_t generation,
                          std::span<const ObjectId> references,
                          std::string_view object_kind) = 0;
    virtual void on_mark_slice(const gc::Heap& heap,
                               std::uint64_t objects_scanned) = 0;
    virtual void on_relocate(const gc::Heap& heap, ObjectId source_id,
                             std::uint64_t size,
                             std::uint8_t source_generation,
                             std::uint64_t from_slot,
                             std::uint64_t to_slot,
                             ObjectId destination_id,
                             TraceMoveKind move_kind) = 0;
    virtual void on_promote(const gc::Heap& heap, ObjectId source_id,
                            std::uint64_t size, std::uint64_t from_slot,
                            std::uint64_t to_slot,
                            ObjectId destination_id) = 0;
    virtual void on_die(const gc::Heap& heap, ObjectId id,
                        std::uint64_t size,
                        std::uint8_t generation) = 0;
    virtual void on_intern(const gc::Heap& heap, ObjectId canonical,
                           std::uint64_t size, std::uint8_t generation,
                           bool hit) = 0;
    virtual void on_evict(const gc::Heap& heap, ObjectId canonical,
                          std::uint64_t size,
                          std::uint8_t generation) = 0;
    virtual void on_trap(const gc::Heap& heap, std::string_view reason) = 0;
    virtual void on_verify_step(
        const gc::Heap& heap, std::string_view check,
        std::optional<std::uint64_t> elements_examined) = 0;

    // GC evidence is observer-only. It is serialized for independent replay
    // and never feeds back into runtime decisions.
    virtual void on_reference_forwarded(ObjectId source_id,
                                        ObjectId destination_id,
                                        TraceForwardKind kind,
                                        std::optional<ObjectId> owner_id) = 0;
    virtual void on_pause_slice() = 0;
    virtual void on_heap_sample(const gc::Heap& heap) = 0;
    virtual void on_logical_collection_begin(
        const gc::Heap& heap, TraceCollectionKind kind) = 0;
    virtual void on_logical_collection_end(const gc::Heap& heap) = 0;
    virtual void on_move_begin(const gc::Heap& heap,
                               TraceMoveCause cause) = 0;
    virtual void on_move_end(const gc::Heap& heap) = 0;
};

class JsonlTraceWriter final : public TraceSink {
public:
    explicit JsonlTraceWriter(
        std::filesystem::path output_directory,
        std::size_t snapshot_interval = 4096);
    ~JsonlTraceWriter() override;

    JsonlTraceWriter(const JsonlTraceWriter&) = delete;
    JsonlTraceWriter& operator=(const JsonlTraceWriter&) = delete;
    JsonlTraceWriter(JsonlTraceWriter&&) noexcept;
    JsonlTraceWriter& operator=(JsonlTraceWriter&&) noexcept;

    void set_mutator_context(
        std::uint64_t tick,
        std::optional<TraceSourcePosition> source_position) override;
    void on_program_start(const gc::Heap& heap) override;
    void on_program_exit(const gc::Heap& heap,
                         std::uint64_t retired_ticks) override;
    void on_alloc(const gc::Heap& heap, ObjectId id, std::uint64_t size,
                  std::uint8_t generation,
                  std::span<const ObjectId> references,
                  std::string_view object_kind) override;
    void on_mark_slice(const gc::Heap& heap,
                       std::uint64_t objects_scanned) override;
    void on_relocate(const gc::Heap& heap, ObjectId source_id,
                     std::uint64_t size, std::uint8_t source_generation,
                     std::uint64_t from_slot, std::uint64_t to_slot,
                     ObjectId destination_id,
                     TraceMoveKind move_kind) override;
    void on_promote(const gc::Heap& heap, ObjectId source_id,
                    std::uint64_t size, std::uint64_t from_slot,
                    std::uint64_t to_slot,
                    ObjectId destination_id) override;
    void on_die(const gc::Heap& heap, ObjectId id, std::uint64_t size,
                std::uint8_t generation) override;
    void on_intern(const gc::Heap& heap, ObjectId canonical,
                   std::uint64_t size, std::uint8_t generation,
                   bool hit) override;
    void on_evict(const gc::Heap& heap, ObjectId canonical,
                  std::uint64_t size,
                  std::uint8_t generation) override;
    void on_trap(const gc::Heap& heap, std::string_view reason) override;
    void on_verify_step(
        const gc::Heap& heap, std::string_view check,
        std::optional<std::uint64_t> elements_examined) override;
    void on_reference_forwarded(ObjectId source_id,
                                ObjectId destination_id,
                                TraceForwardKind kind,
                                std::optional<ObjectId> owner_id) override;
    void on_pause_slice() override;
    void on_heap_sample(const gc::Heap& heap) override;
    void on_logical_collection_begin(
        const gc::Heap& heap, TraceCollectionKind kind) override;
    void on_logical_collection_end(const gc::Heap& heap) override;
    void on_move_begin(const gc::Heap& heap,
                       TraceMoveCause cause) override;
    void on_move_end(const gc::Heap& heap) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::span<const std::string_view> trace_schedule_names();
[[nodiscard]] bool configure_trace_schedule(std::string_view name,
                                            gc::StressConfig& config);

} // namespace lang
