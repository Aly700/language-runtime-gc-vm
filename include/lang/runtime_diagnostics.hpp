#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace lang {

// Runtime diagnostic data deliberately lives in a dependency-light header that
// cannot name Value, ObjectId, Heap, or any collector-owned representation.
struct DebugSourcePosition {
    std::size_t line{1};
    std::size_t column{1};

    bool operator==(const DebugSourcePosition&) const = default;
};

enum class RuntimeFailureKind {
    Trap,
    UncaughtException,
};

struct RuntimeTraceFrame {
    std::size_t function_index{0};
    std::size_t pc{0};
    std::optional<std::string> function_name;
    std::optional<DebugSourcePosition> source_position;

    bool operator==(const RuntimeTraceFrame&) const = default;
};

struct RuntimeTrace {
    RuntimeFailureKind kind{RuntimeFailureKind::Trap};
    std::optional<std::string> exception_variant;
    std::vector<RuntimeTraceFrame> frames;

    bool operator==(const RuntimeTrace&) const = default;
};

static_assert(std::is_standard_layout_v<DebugSourcePosition>);
static_assert(std::is_trivially_copyable_v<DebugSourcePosition>);
static_assert(
    std::is_same_v<decltype(RuntimeTraceFrame::function_index), std::size_t>);
static_assert(std::is_same_v<decltype(RuntimeTraceFrame::pc), std::size_t>);
static_assert(std::is_same_v<decltype(RuntimeTraceFrame::function_name),
                             std::optional<std::string>>);
static_assert(std::is_same_v<decltype(RuntimeTraceFrame::source_position),
                             std::optional<DebugSourcePosition>>);
static_assert(std::is_same_v<decltype(RuntimeTrace::kind),
                             RuntimeFailureKind>);
static_assert(std::is_same_v<decltype(RuntimeTrace::exception_variant),
                             std::optional<std::string>>);
static_assert(std::is_same_v<decltype(RuntimeTrace::frames),
                             std::vector<RuntimeTraceFrame>>);

} // namespace lang
