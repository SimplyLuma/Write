#include <string_view>
#include "engine_types.hpp"

namespace luma::write::engine {

std::string_view format_extension(const DocumentFormat format) noexcept {
  switch (format) {
    case DocumentFormat::docx:
      return "docx";
    case DocumentFormat::odt:
      return "odt";
    case DocumentFormat::legacy_doc:
      return "doc";
    case DocumentFormat::text:
      return "txt";
    case DocumentFormat::pdf:
      return "pdf";
    case DocumentFormat::unknown:
      return {};
  }
  return {};
}

std::string_view format_mime_type(const DocumentFormat format) noexcept {
  switch (format) {
    case DocumentFormat::docx:
      return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    case DocumentFormat::odt:
      return "application/vnd.oasis.opendocument.text";
    case DocumentFormat::legacy_doc:
      return "application/msword";
    case DocumentFormat::text:
      return "text/plain";
    case DocumentFormat::pdf:
      return "application/pdf";
    case DocumentFormat::unknown:
      return "application/octet-stream";
  }
  return "application/octet-stream";
}


CommandInvocation find_invocation(const std::string_view needle,
                                  const bool backwards) {
  // Only the two fields the search needs, escaped, because the needle is
  // whatever the user typed and JSON is not the place to find that out.
  std::string escaped;
  escaped.reserve(needle.size() + 8);
  for (const char character : needle) {
    switch (character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += character; break;
    }
  }
  CommandInvocation invocation;
  invocation.command = EngineCommand::find;
  invocation.arguments =
      std::string{"{\"SearchItem.SearchString\":{\"type\":\"string\",\"value\":\""} +
      escaped +
      "\"},\"SearchItem.Backward\":{\"type\":\"boolean\",\"value\":" +
      (backwards ? "true" : "false") + "}}";
  return invocation;
}

}  // namespace luma::write::engine
