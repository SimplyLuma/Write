#pragma once

#include "engine_types.hpp"

#include <string_view>

namespace luma::write::engine::adapter {

struct CommandMapping {
  std::string_view engine_name;
  bool exposes_state{false};
};

// This is deliberately inside the engine adapter. No toolbar, menu, command
// palette, or inspector may name an engine command directly.
[[nodiscard]] CommandMapping command_mapping(EngineCommand command) noexcept;

}  // namespace luma::write::engine::adapter
