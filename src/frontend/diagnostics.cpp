#include "diagnostics.hpp"

#include <utility>

namespace lang::frontend::detail {

void add_diagnostic(std::vector<Diagnostic>& diagnostics, SourcePosition position,
                    std::string message) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.position.offset == position.offset &&
            diagnostic.message == message) {
            return;
        }
    }
    diagnostics.push_back(Diagnostic{position, std::move(message)});
}

} // namespace lang::frontend::detail
