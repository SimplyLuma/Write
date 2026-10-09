#pragma once

namespace luma::write::app {

// The conform fixture is a separate, read-only process path. It never creates
// a document engine, reads preferences, or opens a user's document.
int run_fixture(const char* path);

}  // namespace luma::write::app
