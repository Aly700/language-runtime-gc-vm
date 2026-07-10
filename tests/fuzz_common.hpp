#pragma once

#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"

#include <cassert>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuzz {

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E37'79B9'7F4A'7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBull;
        return z ^ (z >> 31);
    }

    std::uint64_t bounded(std::uint64_t exclusive_max) {
        assert(exclusive_max > 0);
        return next() % exclusive_max;
    }

    std::int64_t small_i64() {
        return static_cast<std::int64_t>(bounded(81)) - 40;
    }

private:
    std::uint64_t state_;
};

struct Schedule {
    const char* name;
    lang::gc::StressConfig stress;
};

inline std::vector<Schedule> schedules() {
    std::vector<Schedule> result;
    result.push_back({"no_stress", {}});

    lang::gc::StressConfig before_alloc;
    before_alloc.collect_before_every_allocation = true;
    result.push_back({"before_every_alloc", before_alloc});

    lang::gc::StressConfig after_alloc;
    after_alloc.collect_after_every_allocation = true;
    result.push_back({"after_every_alloc", after_alloc});

    for (const auto n : {1ull, 3ull, 7ull}) {
        lang::gc::StressConfig major;
        major.collect_every_n_instructions = n;
        result.push_back({n == 1 ? "major_every_1"
                                 : (n == 3 ? "major_every_3" : "major_every_7"),
                          major});
    }

    for (const auto n : {1ull, 4ull}) {
        lang::gc::StressConfig minor;
        minor.collect_minor_every_n_instructions = n;
        result.push_back({n == 1 ? "minor_every_1" : "minor_every_4", minor});
    }

    lang::gc::StressConfig after_barrier;
    after_barrier.collect_minor_after_every_write_barrier = true;
    result.push_back({"minor_after_every_barrier", after_barrier});

    lang::gc::StressConfig combined;
    combined.collect_before_every_allocation = true;
    combined.collect_after_every_allocation = true;
    combined.collect_every_n_instructions = 7;
    combined.collect_minor_every_n_instructions = 4;
    combined.collect_minor_after_every_write_barrier = true;
    result.push_back({"combined", combined});

    return result;
}

inline const Schedule& find_schedule(const std::vector<Schedule>& all,
                                     const std::string& name) {
    for (const auto& schedule : all) {
        if (schedule.name == name) {
            return schedule;
        }
    }
    std::ostringstream out;
    out << "unknown schedule '" << name << "'. valid schedules:";
    for (const auto& schedule : all) {
        out << " " << schedule.name;
    }
    throw std::runtime_error(out.str());
}

inline std::uint64_t parse_seed(const std::string& value) {
    std::size_t parsed = 0;
    const auto seed = std::stoull(value, &parsed, 10);
    if (parsed != value.size()) {
        throw std::runtime_error("invalid seed: " + value);
    }
    return seed;
}

inline std::string value_token(const lang::gc::Heap& heap, lang::Value value,
                               std::map<lang::ObjectId, std::size_t>& indexes,
                               std::vector<lang::ObjectId>& order) {
    std::ostringstream out;
    switch (value.tag()) {
    case lang::Value::Tag::Int64:
        out << "i64(" << value.as_i64() << ")";
        return out.str();
    case lang::Value::Tag::Bool:
        out << "bool(" << (value.as_bool() ? "true" : "false") << ")";
        return out.str();
    case lang::Value::Tag::Nil:
        return "nil";
    case lang::Value::Tag::Object: {
        const auto id = value.as_object();
        auto it = indexes.find(id);
        if (it == indexes.end()) {
            const auto index = order.size();
            indexes.emplace(id, index);
            order.push_back(id);
            it = indexes.find(id);
        }
        out << "@" << it->second;
        (void)heap.object(id);
        return out.str();
    }
    }
    return "<unknown>";
}

inline std::string canonical_object_graph(const lang::gc::Heap& heap,
                                          lang::ObjectId root) {
    std::map<lang::ObjectId, std::size_t> indexes;
    std::vector<lang::ObjectId> order;
    (void)value_token(heap, lang::Value::object(root), indexes, order);

    std::vector<std::string> objects;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& object = heap.object(order[i]);
        std::ostringstream rendered;
        switch (object.kind) {
        case lang::gc::ObjectKind::Pair:
            rendered << "pair("
                     << value_token(heap, heap.left(order[i]), indexes, order)
                     << ", "
                     << value_token(heap, heap.right(order[i]), indexes, order)
                     << ")";
            break;
        case lang::gc::ObjectKind::ScalarArray:
            rendered << "array[" << heap.array_length(order[i]) << "](";
            for (std::size_t element = 0; element < heap.array_length(order[i]);
                 ++element) {
                if (element != 0) {
                    rendered << ", ";
                }
                rendered << heap.array_get(order[i], element);
            }
            rendered << ")";
            break;
        case lang::gc::ObjectKind::RefArray:
            rendered << "refarray[" << heap.ref_array_length(order[i]) << "](";
            for (std::size_t element = 0; element < heap.ref_array_length(order[i]);
                 ++element) {
                if (element != 0) {
                    rendered << ", ";
                }
                rendered << value_token(heap, heap.ref_array_get(order[i], element),
                                        indexes, order);
            }
            rendered << ")";
            break;
        case lang::gc::ObjectKind::Str: {
            const auto bytes = heap.string_bytes(order[i]);
            rendered << "str[" << bytes.size() << "](";
            for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
                if (byte != 0) {
                    rendered << " ";
                }
                rendered << std::hex << std::setfill('0') << std::setw(2)
                         << static_cast<unsigned>(bytes[byte]) << std::dec;
            }
            rendered << ")";
            break;
        }
        case lang::gc::ObjectKind::Closure:
            rendered << "closure(layout="
                     << heap.closure_layout_index(order[i]) << ", captures=[";
            for (std::size_t capture = 0;
                 capture < heap.closure_capture_count(order[i]); ++capture) {
                if (capture != 0) {
                    rendered << ", ";
                }
                rendered << value_token(
                    heap, heap.closure_capture(order[i], capture), indexes, order);
            }
            rendered << "])";
            break;
        case lang::gc::ObjectKind::Map:
            rendered << "map[" << heap.map_length(order[i]) << "](";
            for (std::size_t entry = 0; entry < heap.map_length(order[i]);
                 ++entry) {
                if (entry != 0) {
                    rendered << ", ";
                }
                rendered << "("
                         << value_token(heap, heap.map_key_at(order[i], entry),
                                        indexes, order)
                         << " => "
                         << value_token(heap, heap.map_value_at(order[i], entry),
                                        indexes, order)
                         << ")";
            }
            rendered << ")";
            break;
        case lang::gc::ObjectKind::WeakRef: {
            const auto target = heap.weak_get(order[i]);
            if (target.tag() == lang::Value::Tag::Nil) {
                rendered << "weak(cleared)";
            } else {
                rendered << "weak(alive="
                         << value_token(heap, target, indexes, order) << ")";
            }
            break;
        }
        }
        objects.push_back(rendered.str());
    }

    std::ostringstream out;
    out << "object(@0)";
    for (std::size_t i = 0; i < objects.size(); ++i) {
        out << "\n  @" << i << " = " << objects[i];
    }
    return out.str();
}

inline std::string observable_for(lang::VM& vm, lang::Value value) {
    vm.heap().TEST_ONLY_validate_gc_invariants();

    std::ostringstream out;
    switch (value.tag()) {
    case lang::Value::Tag::Int64:
        out << "i64:" << value.as_i64();
        break;
    case lang::Value::Tag::Bool:
        out << "bool:" << (value.as_bool() ? "true" : "false");
        break;
    case lang::Value::Tag::Nil:
        out << "nil";
        break;
    case lang::Value::Tag::Object:
        out << canonical_object_graph(vm.heap(), value.as_object());
        break;
    }
    return out.str();
}

struct Outcome {
    bool ok{false};
    std::string observable;
    std::string error;
    std::string output;
};

inline std::string output_for(const lang::VM& vm) {
    return std::string(vm.output().begin(), vm.output().end());
}

inline std::string render_output_bytes(std::string_view output) {
    std::ostringstream rendered;
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (i != 0) {
            rendered << " ";
        }
        rendered << std::hex << std::setfill('0') << std::setw(2)
                 << static_cast<unsigned>(
                        static_cast<unsigned char>(output[i]));
    }
    return rendered.str();
}

inline bool same_observables(const Outcome& baseline, const Outcome& observed) {
    return baseline.observable == observed.observable &&
           baseline.output == observed.output;
}

inline Outcome execute_once(const lang::Function& function, const Schedule& schedule) {
    try {
        lang::Module module;
        module.entry_function = 0;
        module.functions.push_back(function);
        auto verified = lang::verify_module(std::move(module));
        if (!verified.has_value()) {
            return Outcome{false, {}, "bytecode verifier rejected generated function"};
        }
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        const auto value = vm.execute(*verified);
        return Outcome{true, observable_for(vm, value), {}, output_for(vm)};
    } catch (const std::exception& e) {
        return Outcome{false, {}, e.what()};
    }
}

inline Outcome execute_once(const lang::Module& module, const Schedule& schedule) {
    try {
        auto verified = lang::verify_module(module);
        if (!verified.has_value()) {
            return Outcome{false, {}, "bytecode verifier rejected generated module"};
        }
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        const auto value = vm.execute(*verified);
        return Outcome{true, observable_for(vm, value), {}, output_for(vm)};
    } catch (const std::exception& e) {
        return Outcome{false, {}, e.what()};
    }
}

inline Outcome execute_once(const lang::VerifiedModule& module, const Schedule& schedule) {
    try {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        const auto value = vm.execute(module);
        return Outcome{true, observable_for(vm, value), {}, output_for(vm)};
    } catch (const std::exception& e) {
        return Outcome{false, {}, e.what()};
    }
}

} // namespace fuzz
