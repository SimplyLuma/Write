#pragma once

#include "document_engine.hpp"

#include <memory>

namespace luma::write::engine {

[[nodiscard]] std::unique_ptr<DocumentEngine> create_document_engine();

}  // namespace luma::write::engine
