#pragma once

#include "engine/document_engine.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <string_view>

namespace luma::write::app {

using CanvasFactory =
    std::function<GtkWidget*(engine::DocumentEngine& document_engine)>;
using CanvasCommandHandler = std::function<bool(
    GtkWidget* canvas, std::string_view command_id)>;

struct ApplicationOptions {
  CanvasFactory canvas_factory;
  CanvasCommandHandler canvas_command_handler;
};

/** Runs the native desktop application and returns GApplication's exit code. */
int run_application(int argc, char** argv,
                    ApplicationOptions options = ApplicationOptions{});

}  // namespace luma::write::app
