#include "lang/gc/heap.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using lang::Value;
using lang::gc::Heap;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

lang::ObjectId allocate_string(Heap& heap, const std::string& text) {
    return heap.allocate_string(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

template <typename Fn>
void require_out_of_range(Fn&& action, const std::string& message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    require(rejected, message);
}

template <typename Fn>
void require_logic_error(Fn&& action, const std::string& expected,
                         const std::string& message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::logic_error& error) {
        rejected = std::string(error.what()) == expected;
    }
    require(rejected, message);
}

void deterministic_hash_layout_and_probe_counts_are_pinned() {
    Heap heap;
    auto scalar_map = heap.make_handle(heap.allocate_map(0, false, false));

    require(heap.TEST_ONLY_map_lookup_index(scalar_map.object()).empty(),
            "empty map allocated a lookup table");
    for (const auto key : {0, 8, 16, 24}) {
        heap.map_set(scalar_map.object(), Value::int64(key),
                     Value::int64(key + 100));
    }
    const std::vector<std::size_t> four_collision_buckets{
        0, 0, 0, 0, 1, 2, 3, 4,
    };
    require(heap.TEST_ONLY_map_lookup_index(scalar_map.object()) ==
                four_collision_buckets,
            "fixed FNV i64 encoding or linear collision order drifted");

    const auto before_update =
        heap.TEST_ONLY_map_lookup_index(scalar_map.object());
    heap.map_set(scalar_map.object(), Value::int64(8), Value::int64(999));
    require(heap.map_length(scalar_map.object()) == 4 &&
                heap.map_get(scalar_map.object(), Value::int64(8)).as_i64() ==
                    999,
            "existing-key hash update did not replace in place");
    require(heap.TEST_ONLY_map_lookup_index(scalar_map.object()) ==
                before_update,
            "existing-key update changed the lookup index");

    heap.map_set(scalar_map.object(), Value::int64(1), Value::int64(101));
    const std::vector<std::size_t> resized_buckets{
        0, 0, 0, 0, 2, 4, 0, 0,
        0, 0, 0, 0, 1, 3, 5, 0,
    };
    require(heap.TEST_ONLY_map_lookup_index(scalar_map.object()) ==
                resized_buckets,
            "half-load deterministic resize or insertion-order rebuild drifted");
    require(heap.map_key_at(scalar_map.object(), 0).as_i64() == 0 &&
                heap.map_key_at(scalar_map.object(), 1).as_i64() == 8 &&
                heap.map_key_at(scalar_map.object(), 2).as_i64() == 16 &&
                heap.map_key_at(scalar_map.object(), 3).as_i64() == 24 &&
                heap.map_key_at(scalar_map.object(), 4).as_i64() == 1,
            "hash resize changed entry-vector insertion order");

    const auto probes_before = heap.metrics().map_hash_probes;
    const auto comparisons_before =
        heap.metrics().map_lookup_entries_examined;
    require(!heap.map_has(scalar_map.object(), Value::int64(32)),
            "missing colliding i64 key reported present");
    require(heap.metrics().map_hash_probes - probes_before == 4,
            "missing colliding key did not stop at the first empty bucket");
    require(heap.metrics().map_lookup_entries_examined -
                    comparisons_before ==
                3,
            "candidate equality counter did not count occupied probe buckets");

    auto bool_map = heap.make_handle(heap.allocate_map(1, false, false));
    heap.map_set(bool_map.object(), Value::boolean(false), Value::int64(10));
    heap.map_set(bool_map.object(), Value::boolean(true), Value::int64(11));
    const std::vector<std::size_t> bool_buckets{
        0, 0, 0, 0, 2, 0, 0, 1,
    };
    require(heap.TEST_ONLY_map_lookup_index(bool_map.object()) == bool_buckets,
            "fixed FNV bool domain encoding drifted");
    require(heap.map_get(bool_map.object(), Value::boolean(false)).as_i64() ==
                    10 &&
                heap.map_get(bool_map.object(), Value::boolean(true)).as_i64() ==
                    11,
            "bool hash lookup returned the wrong value");

    auto string_map = heap.make_handle(heap.allocate_map(2, true, false));
    for (const auto& [key, value] :
         std::vector<std::pair<std::string, std::int64_t>>{
             {"alpha", 11}, {"beta", 22}, {"gamma", 33}}) {
        const auto object = allocate_string(heap, key);
        heap.map_set(string_map.object(), Value::object(object),
                     Value::int64(value));
    }
    const std::vector<std::size_t> string_buckets{
        2, 0, 1, 3, 0, 0, 0, 0,
    };
    require(heap.TEST_ONLY_map_lookup_index(string_map.object()) ==
                string_buckets,
            "fixed FNV string domain/content encoding drifted");
    const auto fresh_alpha = allocate_string(heap, "alpha");
    require(heap.map_get(string_map.object(), Value::object(fresh_alpha))
                    .as_i64() == 11,
            "fresh byte-equal string missed the content-hash index");
    heap.TEST_ONLY_validate_gc_invariants();
}

void coherence_validator_corruption_hook_is_nonvacuous() {
    {
        Heap heap;
        auto map = heap.make_handle(heap.allocate_map(0, false, false));
        heap.map_set(map.object(), Value::int64(0), Value::int64(10));
        heap.map_set(map.object(), Value::int64(8), Value::int64(20));
        const auto validations_before =
            heap.metrics().map_index_validation_entries;
        heap.TEST_ONLY_validate_gc_invariants();
        require(heap.metrics().map_index_validation_entries >
                    validations_before,
                "map-index validator did not examine ordered entries");
    }

    {
        Heap heap;
        auto map = heap.make_handle(heap.allocate_map(0, false, false));
        heap.map_set(map.object(), Value::int64(0), Value::int64(10));
        heap.map_set(map.object(), Value::int64(8), Value::int64(20));
        heap.TEST_ONLY_corrupt_map_lookup_index(map.object());
        require_logic_error(
            [&] { heap.TEST_ONLY_validate_gc_invariants(); },
            "map lookup index is missing an ordered entry",
            "coherence validator accepted a corrupted lookup bucket");
    }
}

void content_hash_lookup_survives_atomic_compaction() {
    Heap heap;
    (void)heap.allocate_pair(Value::int64(-1), Value::int64(-1));
    auto map = heap.make_handle(heap.allocate_map(4, true, false));
    const auto stored_key = allocate_string(heap, "movement-safe-index");
    heap.map_set(map.object(), Value::object(stored_key), Value::int64(77));
    auto query =
        heap.make_handle(allocate_string(heap, "movement-safe-index"));

    const auto old_map = map.object();
    const auto old_stored_key =
        heap.map_key_at(map.object(), 0).as_object();
    const auto index_before =
        heap.TEST_ONLY_map_lookup_index(map.object());
    heap.collect();

    require(map.object() != old_map,
            "atomic-compaction setup did not move the map owner");
    require(heap.map_key_at(map.object(), 0).as_object() != old_stored_key,
            "atomic-compaction setup did not move the stored string key");
    require(heap.map_get(map.object(), query.value()).as_i64() == 77,
            "content-hash lookup failed after atomic compaction");
    heap.map_set(map.object(), query.value(), Value::int64(88));
    require(heap.map_length(map.object()) == 1 &&
                heap.map_get(map.object(), query.value()).as_i64() == 88,
            "post-compaction byte-equal update appended or lost the value");
    require(heap.TEST_ONLY_map_lookup_index(map.object()) == index_before,
            "movement changed the content-derived bucket layout");
    heap.TEST_ONLY_validate_gc_invariants();
}

void map_growth_relocation_preserves_index_and_stales_old_owner() {
    Heap heap;
    auto map = heap.make_handle(heap.allocate_map(5, false, false));
    heap.map_set(map.object(), Value::int64(0), Value::int64(10));
    (void)heap.allocate_pair(Value::int64(-2), Value::int64(-3));

    const auto old_map = map.object();
    heap.map_set(map.object(), Value::int64(8), Value::int64(20));
    require(map.object() != old_map,
            "blocked adjacent growth did not relocate the map");
    require_out_of_range(
        [&] { (void)heap.map_length(old_map); },
        "raw pre-growth map id did not become stale");
    require(heap.map_get(map.object(), Value::int64(0)).as_i64() == 10 &&
                heap.map_get(map.object(), Value::int64(8)).as_i64() == 20,
            "growth relocation lost a hashed lookup");
    require(heap.map_key_at(map.object(), 0).as_i64() == 0 &&
                heap.map_key_at(map.object(), 1).as_i64() == 8,
            "growth relocation changed insertion order");
    const std::vector<std::size_t> expected{
        0, 0, 0, 0, 1, 2, 0, 0,
    };
    require(heap.TEST_ONLY_map_lookup_index(map.object()) == expected,
            "growth relocation corrupted the collision chain");
    heap.TEST_ONLY_validate_gc_invariants();
}

void lookup_remains_correct_during_incremental_compaction() {
    Heap heap;
    (void)heap.allocate_pair(Value::int64(-4), Value::int64(-5));
    auto map = heap.make_handle(heap.allocate_map(6, true, false));
    const auto stored_key = allocate_string(heap, "incremental-map-key");
    heap.map_set(map.object(), Value::object(stored_key), Value::int64(91));
    auto query =
        heap.make_handle(allocate_string(heap, "incremental-map-key"));
    auto missing =
        heap.make_handle(allocate_string(heap, "incremental-map-missing"));

    const auto index_before =
        heap.TEST_ONLY_map_lookup_index(map.object());
    const auto validation_entries_before =
        heap.metrics().map_index_validation_entries;
    const auto old_map = map.object();
    const auto old_stored_key =
        heap.map_key_at(map.object(), 0).as_object();
    std::size_t step = 0;
    std::size_t map_move_step = 0;
    std::size_t key_move_step = 0;
    heap.start_incremental_compaction();
    while (!heap.incremental_compaction_quiescent()) {
        require(heap.incremental_compact_step(1) == 1,
                "budget-one compaction did not relocate one survivor");
        ++step;
        if (map_move_step == 0 && map.object() != old_map) {
            map_move_step = step;
        }
        if (key_move_step == 0 &&
            heap.map_key_at(map.object(), 0).as_object() !=
                old_stored_key) {
            key_move_step = step;
        }
        require(heap.map_get(map.object(), query.value()).as_i64() == 91,
                "hashed hit failed between compaction relocation steps");
        require(!heap.map_has(map.object(), missing.value()),
                "hashed miss failed between compaction relocation steps");
        heap.TEST_ONLY_validate_gc_invariants();
    }
    heap.finish_incremental_compaction();

    require(heap.map_get(map.object(), query.value()).as_i64() == 91,
            "hashed lookup failed after incremental compaction");
    require(map_move_step != 0 && key_move_step != 0,
            "incremental-compaction setup did not move map and stored key");
    require(map_move_step != key_move_step,
            "budget-one compaction did not separate owner/key relocation");
    require(heap.TEST_ONLY_map_lookup_index(map.object()) == index_before,
            "incremental compaction changed content-derived buckets");
    require(heap.metrics().map_index_validation_entries >
                validation_entries_before,
            "incremental compaction skipped index coherence validation");

    heap.start_incremental_marking();
    while (!heap.incremental_marking_quiescent()) {
        (void)heap.incremental_mark_step(1);
    }
    heap.finish_incremental_marking_to_incremental_compaction();
    while (!heap.incremental_compaction_quiescent()) {
        (void)heap.incremental_compact_step(1);
    }
    heap.finish_incremental_compaction();
    require(heap.map_get(map.object(), query.value()).as_i64() == 91,
            "combined incremental mark/compact changed hashed lookup");
    heap.TEST_ONLY_validate_gc_invariants();
}

} // namespace

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests{
        {"deterministic_hash_layout_and_probe_counts_are_pinned",
         deterministic_hash_layout_and_probe_counts_are_pinned},
        {"coherence_validator_corruption_hook_is_nonvacuous",
         coherence_validator_corruption_hook_is_nonvacuous},
        {"content_hash_lookup_survives_atomic_compaction",
         content_hash_lookup_survives_atomic_compaction},
        {"map_growth_relocation_preserves_index_and_stales_old_owner",
         map_growth_relocation_preserves_index_and_stales_old_owner},
        {"lookup_remains_correct_during_incremental_compaction",
         lookup_remains_correct_during_incremental_compaction},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cerr << "[PASS] " << name << "\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
