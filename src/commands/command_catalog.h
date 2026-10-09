#pragma once

#include "commands/command_registry.h"

#include <vector>

namespace luma::commands {

/** Stable common command vocabulary shared by Write, Grid, and Slides. */
std::vector<CommandDefinition> core_suite_command_definitions();

/** Stable document-editing vocabulary owned by Luma Write. */
std::vector<CommandDefinition> core_write_command_definitions();

/**
 * Registers the common and Write catalogs without handlers. The application
 * binds handlers/state resolvers before exposing a command in UI surfaces.
 */
std::vector<RegistrationResult> register_write_command_catalog(
    CommandRegistry& registry);

}  // namespace luma::commands
