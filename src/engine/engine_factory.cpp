#include "engine_factory.hpp"

#include "luma_config.hpp"
#include "unavailable_engine.hpp"

#if LUMA_HAS_LIBREOFFICEKIT
#include "libreoffice_engine.hpp"
#endif

namespace luma::write::engine {

std::unique_ptr<DocumentEngine> create_document_engine() {
#if LUMA_HAS_LIBREOFFICEKIT
  return std::make_unique<LibreOfficeEngine>();
#else
  return std::make_unique<UnavailableEngine>();
#endif
}

}  // namespace luma::write::engine
