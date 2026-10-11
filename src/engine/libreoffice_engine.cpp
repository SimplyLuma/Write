#include "libreoffice_engine.hpp"

#include "uno_command_map.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.hxx>
#include "engine/office_locations.hpp"

#include <pwd.h>
#include <unistd.h>

namespace luma::write::engine {
namespace {

constexpr int kMaximumTileEdge = 2048;
constexpr std::size_t kPixelBytes = 4;

// LibreOfficeKit's desktop bootstrap is process-global even though its C++
// wrapper looks independently ownable. Keep exactly one owning wrapper alive
// so a second engine cannot destroy the office used by the first one.
std::mutex g_office_lease_mutex;
bool g_office_lease_claimed = false;

[[nodiscard]] bool try_claim_office_lease() noexcept {
  std::lock_guard lock(g_office_lease_mutex);
  if (g_office_lease_claimed) {
    return false;
  }
  g_office_lease_claimed = true;
  return true;
}

void release_office_lease() noexcept {
  std::lock_guard lock(g_office_lease_mutex);
  g_office_lease_claimed = false;
}

[[nodiscard]] bool valid_utf8(const std::string_view text) noexcept {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7FU) {
      ++offset;
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t scalar = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
      continuation_count = 1;
      scalar = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      continuation_count = 2;
      scalar = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      continuation_count = 3;
      scalar = first & 0x07U;
    } else {
      return false;
    }
    if (offset + continuation_count >= text.size()) {
      return false;
    }
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto continuation =
          static_cast<unsigned char>(text[offset + index]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      scalar = (scalar << 6U) | (continuation & 0x3FU);
    }
    if ((continuation_count == 2 && scalar < 0x800U) ||
        (continuation_count == 3 && scalar < 0x10000U) ||
        (scalar >= 0xD800U && scalar <= 0xDFFFU) || scalar > 0x10FFFFU) {
      return false;
    }
    offset += continuation_count + 1U;
  }
  return true;
}

[[nodiscard]] std::optional<std::string> utf8_from_scalar(
    const std::uint32_t scalar) {
  if (scalar > 0x10FFFFU || (scalar >= 0xD800U && scalar <= 0xDFFFU)) {
    return std::nullopt;
  }
  std::string result;
  if (scalar <= 0x7FU) {
    result.push_back(static_cast<char>(scalar));
  } else if (scalar <= 0x7FFU) {
    result.push_back(static_cast<char>(0xC0U | (scalar >> 6U)));
    result.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  } else if (scalar <= 0xFFFFU) {
    result.push_back(static_cast<char>(0xE0U | (scalar >> 12U)));
    result.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  } else {
    result.push_back(static_cast<char>(0xF0U | (scalar >> 18U)));
    result.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  }
  return result;
}

[[nodiscard]] std::string json_string_literal(const std::string_view value) {
  constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                      '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string result;
  result.reserve(value.size() + 2U);
  result.push_back('"');
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\b':
        result += "\\b";
        break;
      case '\f':
        result += "\\f";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (character < 0x20U) {
          result += "\\u00";
          result.push_back(hex[character >> 4U]);
          result.push_back(hex[character & 0x0FU]);
        } else {
          result.push_back(raw_character);
        }
        break;
    }
  }
  result.push_back('"');
  return result;
}

[[nodiscard]] std::string insert_text_arguments(const std::string_view text) {
  return "{\"Text\":{\"type\":\"string\",\"value\":" +
         json_string_literal(text) + "}}";
}

[[nodiscard]] std::optional<std::string> json_string_value(
    const std::string_view json, const std::string_view key) {
  const std::string quoted_key = "\"" + std::string(key) + "\"";
  const auto key_position = json.find(quoted_key);
  if (key_position == std::string_view::npos) {
    return std::nullopt;
  }
  auto cursor = json.find(':', key_position + quoted_key.size());
  if (cursor == std::string_view::npos) {
    return std::nullopt;
  }
  ++cursor;
  while (cursor < json.size() &&
         std::isspace(static_cast<unsigned char>(json[cursor])) != 0) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != '"') {
    return std::nullopt;
  }
  const auto end = json.find('"', cursor + 1U);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  return std::string(json.substr(cursor + 1U, end - cursor - 1U));
}

[[nodiscard]] bool is_url_unreserved(const unsigned char value) noexcept {
  const bool ascii_alphanumeric =
      (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9');
  return ascii_alphanumeric || value == '-' || value == '_' || value == '.' ||
         value == '~' || value == '/';
}

[[nodiscard]] std::string file_url(const std::filesystem::path& input) {
  const auto absolute = std::filesystem::absolute(input).lexically_normal().generic_string();
  constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                      '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string result = "file://";
  result.reserve(result.size() + absolute.size() + 8);
  for (const char raw_value : absolute) {
    const auto value = static_cast<unsigned char>(raw_value);
    if (is_url_unreserved(value)) {
      result.push_back(static_cast<char>(value));
    } else {
      result.push_back('%');
      result.push_back(hex[value >> 4U]);
      result.push_back(hex[value & 0x0FU]);
    }
  }
  return result;
}

[[nodiscard]] int checked_int(const std::int64_t value) noexcept {
  return static_cast<int>(std::clamp<std::int64_t>(value, std::numeric_limits<int>::min(),
                                                   std::numeric_limits<int>::max()));
}

[[nodiscard]] std::vector<std::int64_t> parse_integers(std::string_view text,
                                                       const std::size_t limit) {
  std::vector<std::int64_t> numbers;
  numbers.reserve(limit);
  std::size_t cursor = 0;
  while (cursor < text.size() && numbers.size() < limit) {
    while (cursor < text.size() && text[cursor] != '-' &&
           std::isdigit(static_cast<unsigned char>(text[cursor])) == 0) {
      ++cursor;
    }
    if (cursor >= text.size()) {
      break;
    }
    const std::size_t start = cursor;
    if (text[cursor] == '-') {
      ++cursor;
    }
    while (cursor < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }
    std::int64_t value = 0;
    const auto conversion = std::from_chars(text.data() + start, text.data() + cursor, value);
    if (conversion.ec == std::errc{}) {
      numbers.push_back(value);
    }
  }
  return numbers;
}

[[nodiscard]] std::optional<Rect> parse_rectangle(const std::string_view payload) {
  if (payload.empty() || payload.find("EMPTY") != std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view rectangle_text = payload;
  const auto rectangle_key = payload.find("\"rectangle\"");
  if (rectangle_key != std::string_view::npos) {
    rectangle_text = payload.substr(rectangle_key + 11);
  }
  const auto values = parse_integers(rectangle_text, 4);
  if (values.size() != 4) {
    return std::nullopt;
  }
  return Rect{values[0], values[1], values[2], values[3]};
}

[[nodiscard]] std::vector<Rect> parse_rectangles(const std::string_view payload) {
  std::vector<Rect> result;
  std::size_t beginning = 0;
  while (beginning <= payload.size()) {
    const auto end = payload.find(';', beginning);
    const auto segment = payload.substr(beginning, end == std::string_view::npos
                                                       ? std::string_view::npos
                                                       : end - beginning);
    if (const auto rectangle = parse_rectangle(segment); rectangle.has_value()) {
      result.push_back(*rectangle);
    }
    if (end == std::string_view::npos) {
      break;
    }
    beginning = end + 1;
  }
  return result;
}

[[nodiscard]] DocumentFormat format_from_path(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(), [](const char raw_character) {
    const auto character = static_cast<unsigned char>(raw_character);
    return static_cast<char>(std::tolower(character));
  });
  if (extension == ".docx") {
    return DocumentFormat::docx;
  }
  if (extension == ".odt") {
    return DocumentFormat::odt;
  }
  if (extension == ".doc") {
    return DocumentFormat::legacy_doc;
  }
  if (extension == ".txt") {
    return DocumentFormat::text;
  }
  if (extension == ".pdf") {
    return DocumentFormat::pdf;
  }
  return DocumentFormat::unknown;
}

[[nodiscard]] bool source_path_writable(const std::filesystem::path& path) {
  const auto parent = path.has_parent_path() ? path.parent_path()
                                             : std::filesystem::path{"."};
  return ::access(path.c_str(), W_OK) == 0 &&
         ::access(parent.c_str(), W_OK) == 0;
}

[[nodiscard]] CommandState state_from_payload(const std::string_view value) {
  if (value == "true" || value == "1") {
    return {CommandStateKind::on, {}};
  }
  if (value == "false" || value == "0") {
    return {CommandStateKind::off, {}};
  }
  if (value == "disabled") {
    return {CommandStateKind::unavailable, {}};
  }
  if (value == "mixed" || value == "indeterminate") {
    return {CommandStateKind::mixed, {}};
  }
  return {CommandStateKind::value, std::string(value)};
}

[[nodiscard]] bool command_mutates_document(const EngineCommand command) {
  switch (command) {
    case EngineCommand::select_all:
    case EngineCommand::find:
    case EngineCommand::previous_change:
    case EngineCommand::next_change:
      return false;
    case EngineCommand::undo:
    case EngineCommand::redo:
    case EngineCommand::cut:
    case EngineCommand::bold:
    case EngineCommand::italic:
    case EngineCommand::underline:
    case EngineCommand::strikethrough:
    case EngineCommand::superscript:
    case EngineCommand::subscript:
    case EngineCommand::align_left:
    case EngineCommand::align_center:
    case EngineCommand::align_right:
    case EngineCommand::align_justify:
    case EngineCommand::toggle_bullets:
    case EngineCommand::toggle_numbering:
    case EngineCommand::clear_character_overrides:
    case EngineCommand::clear_paragraph_overrides:
    case EngineCommand::apply_paragraph_style:
    case EngineCommand::insert_table:
    case EngineCommand::insert_image:
    case EngineCommand::insert_page_break:
    case EngineCommand::replace:
    case EngineCommand::track_changes:
    case EngineCommand::accept_change:
    case EngineCommand::reject_change:
    case EngineCommand::add_comment:
    case EngineCommand::update_table_of_contents:
      return true;
  }
  return false;
}

[[nodiscard]] std::vector<std::string> quoted_strings(const std::string_view payload) {
  std::vector<std::string> result;
  bool escaped = false;
  bool inside = false;
  std::string current;
  for (const char character : payload) {
    if (!inside) {
      if (character == '"') {
        inside = true;
        current.clear();
      }
      continue;
    }
    if (escaped) {
      current.push_back(character);
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else if (character == '"') {
      inside = false;
      if (!current.empty() && current != "fonts" && current != "missingFonts") {
        result.push_back(current);
      }
    } else {
      current.push_back(character);
    }
  }
  return result;
}


// Write's page themes, as LibreOffice colour schemes in the private profile.
// A scheme is view state: it decides how the page is painted on screen and is
// never stored in the document, so printing and export keep real colours.
// Moon's page and ink match kMoonPage/kMoonInk in the canvas.
constexpr const char* kSunScheme = "LumaSun";
constexpr const char* kMoonScheme = "LumaMoon";

[[nodiscard]] std::string colour_scheme_item(const char* name,
                                             const std::vector<std::pair<const char*, long>>& colours) {
  std::string item = "<item oor:path=\"/org.openoffice.Office.UI/ColorScheme/ColorSchemes\">"
                     "<node oor:name=\"";
  item += name;
  item += "\" oor:op=\"replace\">";
  for (const auto& [entry, colour] : colours) {
    item += "<node oor:name=\"";
    item += entry;
    item += "\"><prop oor:name=\"Color\"><value>" + std::to_string(colour) +
            "</value></prop></node>";
  }
  item += "</node></item>\n";
  return item;
}

void seed_private_profile(const std::filesystem::path& profile) {
  std::error_code error;
  const auto user = profile / "user";
  std::filesystem::create_directories(user, error);
  const auto registry = user / "registrymodifications.xcu";
  if (error || std::filesystem::exists(registry, error)) {
    return;
  }
  std::string text =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<oor:items xmlns:oor=\"http://openoffice.org/2001/registry\" "
      "xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n";
  text += "<item oor:path=\"/org.openoffice.Office.Writer/Content/Display\">"
          "<prop oor:name=\"TextBoundaries\" oor:op=\"fuse\"><value>false</value></prop>"
          "</item>\n";
  // Comments and tracked changes carry the person's name, not "Unknown
  // Author".
  std::string real_name;
  if (const passwd* account = ::getpwuid(::getuid());
      account != nullptr && account->pw_gecos != nullptr) {
    real_name = account->pw_gecos;
    real_name = real_name.substr(0, real_name.find(','));
  }
  if (real_name.empty()) {
    if (const char* login = std::getenv("USER"); login != nullptr) real_name = login;
  }
  if (!real_name.empty() && valid_utf8(real_name)) {
    std::string escaped;
    for (const char character : real_name) {
      switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default: escaped.push_back(character);
      }
    }
    text += "<item oor:path=\"/org.openoffice.UserProfile/Data\"><prop oor:name=\"givenname\" "
            "oor:op=\"fuse\"><value>" + escaped + "</value></prop></item>\n";
  }
  text += colour_scheme_item(kSunScheme, {{"DocColor", 0xffffff}, {"AppBackground", 0xffffff}});
  text += colour_scheme_item(kMoonScheme, {{"DocColor", 0x31353b},
                                            {"AppBackground", 0x31353b},
                                            {"DocBoundaries", 0x31353b},
                                            {"FontColor", 0xe3e6e9},
                                            {"Links", 0x9dbde0},
                                            {"LinksVisited", 0xc0a4d8},
                                            {"WriterFieldShadings", 0x41464e},
                                            {"WriterIdxShadings", 0x41464e},
                                            {"WriterSectionBoundaries", 0x4a5058},
                                            {"WriterHeaderFooterMark", 0x6a727b},
                                            {"WriterPageBreaks", 0x6a727b},
                                            {"WriterNonPrintChars", 0x8a99a8},
                                            {"Shadow", 0x31353b}});
  text += "</oor:items>\n";
  std::ofstream stream(registry, std::ios::binary | std::ios::trunc);
  stream << text;
}

// LibreOfficeKit turns spelling and hyphenation off unless it is told which
// languages to load. Allow the person's own languages that have a dictionary
// installed, and US English, which every Luma image carries.
void allow_spelling_languages() {
  if (std::getenv("LOK_ALLOWLIST_LANGUAGES") != nullptr) {
    return;
  }
  std::vector<std::string> wanted;
  for (const char* variable : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* value = std::getenv(variable);
    if (value == nullptr || *value == '\0') {
      continue;
    }
    std::string list{value};
    std::size_t start = 0;
    while (start <= list.size()) {
      const auto end = list.find(':', start);
      std::string locale = list.substr(start, end == std::string::npos ? std::string::npos
                                                                       : end - start);
      locale = locale.substr(0, locale.find_first_of(".@"));
      if (!locale.empty() && locale != "C" && locale != "POSIX") {
        wanted.push_back(locale);
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  wanted.emplace_back("en_US");
  std::string allowed;
  std::error_code error;
  for (const auto& locale : wanted) {
    const bool installed =
        std::filesystem::exists("/usr/share/hunspell/" + locale + ".dic", error) ||
        std::filesystem::exists("/usr/share/myspell/" + locale + ".dic", error);
    if (!installed || allowed.find(locale) != std::string::npos) {
      continue;
    }
    if (!allowed.empty()) allowed += ' ';
    allowed += locale;
  }
  if (!allowed.empty()) {
    ::setenv("LOK_ALLOWLIST_LANGUAGES", allowed.c_str(), 0);
  }
}

// LibreOffice leaves a lock beside a document it has open, and opens the file
// read-only whenever it finds one. A Write session that ended abruptly leaves
// its lock behind, which silently made the next Write open that document
// read-only. A lock is only removed when it is provably another Write
// session's (same user and host, a Write private profile that is not ours);
// anyone else's lock is respected.
// Returns true when a lock that is not a stale Write lock remains: someone
// else has the document open, and the engine will open it read-only.
bool clear_stale_write_lock(const std::filesystem::path& document,
                            const std::optional<std::filesystem::path>& own_profile) {
  const auto lock = document.parent_path() /
                    (".~lock." + document.filename().string() + "#");
  std::error_code error;
  if (!std::filesystem::is_regular_file(lock, error)) {
    return false;
  }
  std::ifstream stream(lock);
  std::string line;
  std::getline(stream, line);
  std::vector<std::string> fields;
  std::string field;
  for (const char character : line) {
    if (character == ',' || character == ';') {
      fields.push_back(field);
      field.clear();
    } else {
      field.push_back(character);
    }
  }
  fields.push_back(field);
  if (fields.size() < 5) {
    return true;
  }
  // The account name, from the password database: USER is not set in every
  // session (a container, a service).
  const char* user = std::getenv("USER");
  if (const passwd* account = ::getpwuid(::getuid()); account != nullptr) {
    user = account->pw_name;
  }
  char host[256] = {};
  if (::gethostname(host, sizeof(host) - 1) != 0) {
    return true;
  }
  const std::string& profile = fields[4];
  const bool same_user = user != nullptr && fields[1] == user;
  const bool same_host = fields[2] == host;
  const bool write_profile = profile.find("/luma-write-lok-") != std::string::npos;
  const bool ours = own_profile.has_value() &&
                    profile.find(own_profile->filename().string()) != std::string::npos;
  if (same_user && same_host && write_profile && !ours) {
    std::filesystem::remove(lock, error);
    return static_cast<bool>(error);
  }
  return !ours;
}

[[nodiscard]] std::string decode_xml_text(std::string_view xml) {
  std::string text;
  std::size_t index = 0;
  while (index < xml.size()) {
    const char character = xml[index];
    if (character == '<') {
      const auto close = xml.find('>', index);
      if (close == std::string_view::npos) {
        break;
      }
      const auto tag = xml.substr(index, close - index + 1);
      if (tag.starts_with("<text:s") || tag.starts_with("<text:tab") ||
          tag.starts_with("<text:line-break")) {
        text.push_back(' ');
      }
      index = close + 1;
      continue;
    }
    if (character == '&') {
      const auto end = xml.find(';', index);
      if (end != std::string_view::npos && end - index <= 8) {
        const auto entity = xml.substr(index, end - index + 1);
        if (entity == "&amp;") text.push_back('&');
        else if (entity == "&lt;") text.push_back('<');
        else if (entity == "&gt;") text.push_back('>');
        else if (entity == "&quot;") text.push_back('"');
        else if (entity == "&apos;") text.push_back('\'');
        index = end + 1;
        continue;
      }
    }
    text.push_back(character);
    ++index;
  }
  // Collapse runs of whitespace the way the page shows them.
  std::string collapsed;
  bool space = false;
  for (const char character : text) {
    if (character == ' ' || character == '\n' || character == '\t' || character == '\r') {
      space = !collapsed.empty();
    } else {
      if (space) collapsed.push_back(' ');
      space = false;
      collapsed.push_back(character);
    }
  }
  return collapsed;
}

[[nodiscard]] std::vector<OutlineHeading> headings_from_flat_odt(std::string_view xml) {
  std::vector<OutlineHeading> headings;
  const auto body = xml.find("<office:body");
  std::size_t cursor = body == std::string_view::npos ? 0 : body;
  while (true) {
    const auto start = xml.find("<text:h", cursor);
    if (start == std::string_view::npos) {
      break;
    }
    const auto tag_end = xml.find('>', start);
    if (tag_end == std::string_view::npos) {
      break;
    }
    const auto tag = xml.substr(start, tag_end - start);
    int level = 1;
    if (const auto at = tag.find("text:outline-level=\""); at != std::string_view::npos) {
      level = std::clamp(std::atoi(std::string(tag.substr(at + 20, 2)).c_str()), 1, 10);
    }
    if (tag.ends_with("/")) {
      cursor = tag_end + 1;
      continue;
    }
    const auto close = xml.find("</text:h>", tag_end);
    if (close == std::string_view::npos) {
      break;
    }
    auto text = decode_xml_text(xml.substr(tag_end + 1, close - tag_end - 1));
    if (!text.empty()) {
      headings.push_back({std::move(text), level});
    }
    cursor = close + 9;
  }
  return headings;
}

}  // namespace

class LibreOfficeEngine::Impl {
 public:
  Result initialize(const EngineConfiguration& configuration);
  Result load(std::string_view url, std::optional<std::filesystem::path> source_path);
  Result require_document() const;
  Result require_owner_thread() const;
  Result insert_text(std::string_view text);
  void mark_modified();
  void mark_command_mutation(bool modifies_document);
  bool enter_callback() noexcept;
  void leave_callback() noexcept;
  void wait_for_callbacks() noexcept;
  void release_document() noexcept;
  void shutdown() noexcept;
  [[nodiscard]] std::string last_error(std::string_view prefix) const;
  void update_document_geometry();
  void handle_callback(int type, const char* payload);
  void handle_office_callback(int type, const char* payload);
  void emit(EngineEvent event) const;

  static void document_callback(int type, const char* payload, void* data) {
    auto* self = static_cast<Impl*>(data);
    if (!self->enter_callback()) {
      return;
    }
    try {
      self->handle_callback(type, payload);
    } catch (...) {
      // Never allow an allocation or parsing failure to unwind across LOK's C
      // callback boundary.
    }
    self->leave_callback();
  }

  static void office_callback(int type, const char* payload, void* data) {
    auto* self = static_cast<Impl*>(data);
    if (!self->enter_callback()) {
      return;
    }
    try {
      self->handle_office_callback(type, payload);
    } catch (...) {
      // Never allow an allocation or parsing failure to unwind across LOK's C
      // callback boundary.
    }
    self->leave_callback();
  }

  EngineConfiguration configuration;
  std::unique_ptr<lok::Office> office;
  std::unique_ptr<lok::Document> document;
  EventSink sink;
  DocumentStatus document_status;
  std::string version;
  std::thread::id owner_thread;
  mutable std::mutex state_mutex;
  std::mutex callback_mutex;
  std::condition_variable callbacks_drained;
  std::size_t active_callback_count{0};
  bool callbacks_enabled{false};
  bool office_lease_claimed{false};
  std::unordered_map<std::string, CommandState> command_states;
  // Every state the engine reports, verbatim, for the inspector and status.
  std::unordered_map<std::string, std::string> raw_states;
  ViewTheme view_theme{ViewTheme::sun};
  int last_window_id{0};
  bool dialog_open{false};
  std::atomic<bool> graphic_selected{false};
  std::string pending_page_part_removal;
  bool quiet_progress{false};
  void apply_view_theme();
  bool view_theme_applied{false};
};

Result LibreOfficeEngine::Impl::require_owner_thread() const {
  if (owner_thread != std::this_thread::get_id()) {
    return Result::failure("LibreOfficeKit access attempted from a non-owner thread");
  }
  return Result::success();
}

Result LibreOfficeEngine::Impl::require_document() const {
  if (const auto thread_result = require_owner_thread(); !thread_result.ok) {
    return thread_result;
  }
  if (!document) {
    return Result::failure("No document is open");
  }
  return Result::success();
}

void LibreOfficeEngine::Impl::mark_modified() {
  {
    std::lock_guard lock(state_mutex);
    document_status.modified = true;
  }
  emit({.kind = EngineEventKind::document_modified});
}

Result LibreOfficeEngine::Impl::insert_text(const std::string_view text) {
  if (text.empty()) {
    return Result::success();
  }
  if (!valid_utf8(text)) {
    return Result::failure("Text input is not valid UTF-8");
  }
  const std::string arguments = insert_text_arguments(text);
  mark_command_mutation(true);
  // In the pinned LibreOffice 26.2 desktop implementation, requesting a UNO
  // result forces synchronous Sfx dispatch: the model edit is complete when
  // this call returns. The result callback itself is UI-queued, so it must not
  // be used as a blocking fence on the owner thread.
  document->postUnoCommand(".uno:InsertText", arguments.c_str(), true);
  return Result::success();
}

void LibreOfficeEngine::Impl::mark_command_mutation(
    const bool modifies_document) {
  if (modifies_document) {
    mark_modified();
  }
}

bool LibreOfficeEngine::Impl::enter_callback() noexcept {
  std::lock_guard lock(callback_mutex);
  if (!callbacks_enabled) {
    return false;
  }
  ++active_callback_count;
  return true;
}

void LibreOfficeEngine::Impl::leave_callback() noexcept {
  {
    std::lock_guard lock(callback_mutex);
    if (active_callback_count > 0) {
      --active_callback_count;
    }
  }
  callbacks_drained.notify_all();
}

void LibreOfficeEngine::Impl::wait_for_callbacks() noexcept {
  std::unique_lock lock(callback_mutex);
  callbacks_drained.wait(lock,
                         [this] { return active_callback_count == 0; });
}

void LibreOfficeEngine::Impl::release_document() noexcept {
  if (!document) {
    return;
  }
  document->registerCallback(nullptr, nullptr);
  wait_for_callbacks();
  graphic_selected.store(false);
  document.reset();
}

void LibreOfficeEngine::Impl::shutdown() noexcept {
  {
    std::lock_guard lock(state_mutex);
    sink = {};
  }
  {
    std::lock_guard lock(callback_mutex);
    callbacks_enabled = false;
  }
  if (office) {
    office->registerCallback(nullptr, nullptr);
  }
  release_document();
  wait_for_callbacks();
  office.reset();
  if (office_lease_claimed) {
    release_office_lease();
    office_lease_claimed = false;
  }
}

std::string LibreOfficeEngine::Impl::last_error(const std::string_view prefix) const {
  std::string message(prefix);
  if (!office) {
    return message;
  }
  char* raw_error = office->getError();
  if (raw_error != nullptr) {
    if (*raw_error != '\0') {
      message += ": ";
      message += raw_error;
    }
    office->freeError(raw_error);
  }
  return message;
}

Result LibreOfficeEngine::Impl::initialize(const EngineConfiguration& requested_configuration) {
  if (office) {
    return Result::success();
  }
  owner_thread = std::this_thread::get_id();
  configuration = requested_configuration;

  std::vector<std::filesystem::path> candidates;
  if (configuration.office_program_directory.has_value()) {
    candidates.push_back(*configuration.office_program_directory);
  }
  if (const char* environment_path = std::getenv("LUMA_LIBREOFFICE_PROGRAM");
      environment_path != nullptr && *environment_path != '\0') {
    candidates.emplace_back(environment_path);
  }
  luma::office::add_platform_candidates(candidates);

  std::string profile_url;
  if (configuration.private_profile_directory.has_value()) {
    std::error_code error;
    std::filesystem::create_directories(*configuration.private_profile_directory, error);
    if (error) {
      return Result::failure("Could not create the private document-engine profile: " +
                             error.message());
    }
    seed_private_profile(*configuration.private_profile_directory);
    profile_url = file_url(*configuration.private_profile_directory);
  }

  if (!try_claim_office_lease()) {
    return Result::failure(
        "LibreOfficeKit is process-global; this MVP supports one active "
        "document engine at a time");
  }
  office_lease_claimed = true;

  allow_spelling_languages();
  for (const auto& candidate : candidates) {
    if (!luma::office::looks_like_program_dir(candidate)) {
      continue;
    }
    office.reset(lok::lok_cpp_init(candidate.c_str(), profile_url.empty() ? nullptr
                                                                         : profile_url.c_str()));
    if (office) {
      break;
    }
  }
  if (!office) {
    release_office_lease();
    office_lease_claimed = false;
#if defined(__ANDROID__) || defined(__APPLE__)
    return Result::failure("Could not initialize LibreOfficeKit. " + luma::office::where_to_install());
#else
    return Result::failure(
        "Could not initialize the Fedora LibreOfficeKit installation. Expected "
        "libsofficeapp.so under /usr/lib64/libreoffice/program or "
        "LUMA_LIBREOFFICE_PROGRAM.");
#endif
  }

  if (char* version_info = office->getVersionInfo(); version_info != nullptr) {
    version = version_info;
    office->freeError(version_info);
  }
  const auto product_version = json_string_value(version, "ProductVersion");
  const bool qualified_version =
      product_version.has_value() &&
      (*product_version == "26.2" || product_version->starts_with("26.2."));
  if (!qualified_version) {
    const std::string reported_version =
        product_version.value_or("an unreported version");
    office.reset();
    release_office_lease();
    office_lease_claimed = false;
    return Result::failure(
        "This MVP is qualified only for LibreOfficeKit 26.2.x, but found " +
        reported_version);
  }

  {
    std::lock_guard lock(callback_mutex);
    callbacks_enabled = true;
  }
  office->registerCallback(&Impl::office_callback, this);
  office->setOptionalFeatures(LOK_FEATURE_DOCUMENT_PASSWORD |
                              LOK_FEATURE_DOCUMENT_PASSWORD_TO_MODIFY |
                              LOK_FEATURE_PART_IN_INVALIDATION_CALLBACK |
                              LOK_FEATURE_VIEWID_IN_VISCURSOR_INVALIDATION_CALLBACK |
                              LOK_FEATURE_NO_TILED_ANNOTATIONS);
  return Result::success();
}

Result LibreOfficeEngine::Impl::load(const std::string_view url,
                                     std::optional<std::filesystem::path> source_path) {
  if (const auto thread_result = require_owner_thread(); !thread_result.ok) {
    return thread_result;
  }
  if (!office) {
    return Result::failure("The document engine has not been initialized");
  }
  release_document();
  const bool locked_elsewhere =
      source_path.has_value() &&
      clear_stale_write_lock(*source_path, configuration.private_profile_directory);
  const char* load_options =
      configuration.disable_active_content ? "MacroExecutionMode=0,UpdateDocMode=0" : nullptr;
  graphic_selected.store(false);
  document.reset(office->documentLoad(std::string(url).c_str(), load_options));
  if (!document) {
    return Result::failure(last_error("Could not open the document"));
  }
  if (document->getDocumentType() != LOK_DOCTYPE_TEXT) {
    graphic_selected.store(false);
  document.reset();
    return Result::failure("The selected file is not a text document");
  }

  {
    std::lock_guard lock(state_mutex);
    document_status = {};
    document_status.identity.original_path = source_path;
    document_status.revision = 1;
    document_status.read_only = locked_elsewhere;
    if (source_path.has_value()) {
      document_status.identity.extension = source_path->extension().string();
      document_status.identity.save_format = format_from_path(*source_path);
      document_status.identity.mime_type =
          std::string(format_mime_type(document_status.identity.save_format));
      document_status.source_writable = source_path_writable(*source_path);
    } else {
      document_status.identity.extension = ".docx";
      document_status.identity.mime_type = std::string(format_mime_type(DocumentFormat::docx));
      document_status.identity.save_format = DocumentFormat::docx;
      document_status.modified = true;
    }
    command_states.clear();
    raw_states.clear();
  }
  // Initialize the view before installing its callback, and never clear
  // formatting facts delivered during callback/accessibility setup.
  document->initializeForRendering("{}");
  document->registerCallback(&Impl::document_callback, this);
  if (configuration.enable_accessibility) {
    document->setAccessibilityState(document->getView(), true);
  }
  update_document_geometry();
  // A scheme applies to the view that asks for it, so every new view asks.
  apply_view_theme();
  return Result::success();
}

void LibreOfficeEngine::Impl::apply_view_theme() {
  // Sun is the engine's own default; only a dark scheme needs asking for.
  if (!document || (view_theme == ViewTheme::sun && !view_theme_applied)) {
    return;
  }
  view_theme_applied = view_theme == ViewTheme::moon;
  const std::string arguments =
      std::string{"{\"NewTheme\":{\"type\":\"string\",\"value\":\""} +
      (view_theme == ViewTheme::moon ? kMoonScheme : kSunScheme) + "\"}}";
  document->postUnoCommand(".uno:ChangeTheme", arguments.c_str(), true);
}

void LibreOfficeEngine::Impl::update_document_geometry() {
  if (!document) {
    return;
  }
  long width = 0;
  long height = 0;
  document->getDocumentSize(&width, &height);
  int pages = 1;
  std::vector<Rect> page_areas;
  if (char* rectangles = document->getPartPageRectangles(); rectangles != nullptr) {
    const std::string_view value(rectangles);
    if (!value.empty()) {
      page_areas = parse_rectangles(value);
      pages = std::max(1, static_cast<int>(page_areas.size()));
    }
    office->freeError(rectangles);
  }
  std::lock_guard lock(state_mutex);
  document_status.extent_twips = {static_cast<std::int64_t>(width),
                                  static_cast<std::int64_t>(height)};
  document_status.page_areas_twips = std::move(page_areas);
  document_status.page_count = std::max(1, pages);
}

void LibreOfficeEngine::Impl::emit(EngineEvent event) const {
  EventSink copied_sink;
  {
    std::lock_guard lock(state_mutex);
    copied_sink = sink;
    event.revision = document_status.revision;
  }
  if (copied_sink) {
    copied_sink(event);
  }
}

void LibreOfficeEngine::Impl::handle_office_callback(const int type, const char* raw_payload) {
  const std::string payload = raw_payload == nullptr ? std::string{} : std::string(raw_payload);
  switch (type) {
    case LOK_CALLBACK_DOCUMENT_PASSWORD:
    case LOK_CALLBACK_DOCUMENT_PASSWORD_TO_MODIFY:
      emit({.kind = EngineEventKind::password_requested, .text = payload});
      // Loading occurs synchronously on the engine thread. Until the UI/engine
      // split can service this callback asynchronously, abort safely rather
      // than logging, persisting, guessing, or repeatedly trying a password.
      office->setDocumentPassword(payload.c_str(), nullptr);
      break;
    case LOK_CALLBACK_STATUS_INDICATOR_START:
      if (quiet_progress) break;
      emit({.kind = EngineEventKind::progress, .text = payload, .numeric_value = 0});
      break;
    case LOK_CALLBACK_STATUS_INDICATOR_SET_VALUE: {
      if (quiet_progress) break;
      const auto values = parse_integers(payload, 1);
      emit({.kind = EngineEventKind::progress,
            .text = {},
            .numeric_value = values.empty() ? 0 : checked_int(values.front())});
      break;
    }
    case LOK_CALLBACK_ERROR:
      emit({.kind = EngineEventKind::error, .text = payload});
      break;
    default:
      break;
  }
}

void LibreOfficeEngine::Impl::handle_callback(const int type, const char* raw_payload) {
  const std::string payload = raw_payload == nullptr ? std::string{} : std::string(raw_payload);
  switch (type) {
    case LOK_CALLBACK_INVALIDATE_TILES: {
      Revision revision = 0;
      {
        std::lock_guard lock(state_mutex);
        revision = ++document_status.revision;
      }
      EngineEvent event{.kind = EngineEventKind::tiles_invalidated,
                        .area_twips = parse_rectangle(payload),
                        .revision = revision};
      emit(std::move(event));
      break;
    }
    case LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR: {
      emit({.kind = EngineEventKind::cursor_changed,
            .area_twips = parse_rectangle(payload),
            .text = payload});
      break;
    }
    case LOK_CALLBACK_CURSOR_VISIBLE:
      emit({.kind = EngineEventKind::cursor_visibility_changed,
            .numeric_value = payload == "true" ? 1 : 0});
      break;
    case LOK_CALLBACK_TEXT_SELECTION:
      emit({.kind = EngineEventKind::text_selection_changed,
            .areas_twips = parse_rectangles(payload)});
      break;
    case LOK_CALLBACK_GRAPHIC_SELECTION:
    {
      // "x, y, width, height, angle, {handles}": the rectangle is the leading
      // four numbers; the handles' JSON has a "rectangle" key of its own.
      const auto selection =
          parse_rectangle(std::string_view{payload}.substr(0, payload.find('{')));
      graphic_selected.store(selection.has_value());
      emit({.kind = EngineEventKind::graphic_selection_changed,
            .area_twips = selection,
            .text = payload});
      break;
    }
    case LOK_CALLBACK_DOCUMENT_SIZE_CHANGED:
      update_document_geometry();
      emit({.kind = EngineEventKind::document_extent_changed,
            .area_twips = parse_rectangle("0,0," + payload)});
      break;
    case LOK_CALLBACK_STATE_CHANGED: {
      std::string command;
      std::string raw;
      if (!payload.empty() && payload.front() == '{') {
        // Some states arrive as JSON: {"commandName": ..., "state": ...}.
        command = json_string_value(payload, "commandName").value_or("");
        raw = json_string_value(payload, "state").value_or("");
        if (payload.find("\"disabled\": true") != std::string::npos) {
          raw = "disabled";
        }
      } else {
        const auto equals = payload.find('=');
        if (equals == std::string::npos) {
          break;
        }
        command = payload.substr(0, equals);
        raw = payload.substr(equals + 1);
      }
      if (command.empty()) {
        break;
      }
      const auto state = state_from_payload(raw);
      {
        std::lock_guard lock(state_mutex);
        command_states[command] = state;
        raw_states[command] = raw;
      }
      emit({.kind = EngineEventKind::command_state_changed,
            .command_id = command,
            .command_state = state});
      break;
    }
    case LOK_CALLBACK_UNO_COMMAND_RESULT: {
      emit({.kind = EngineEventKind::command_completed, .text = payload});
      break;
    }
    case LOK_CALLBACK_CONTEXT_CHANGED:
      emit({.kind = EngineEventKind::context_changed, .text = payload});
      break;
    // The payload carries the match rectangles and a highlight index, which is
    // what lets the find bar report a count the engine actually knows rather
    // than a number the interface made up.
    case LOK_CALLBACK_SEARCH_RESULT_SELECTION:
      emit({.kind = EngineEventKind::search_result, .text = payload});
      break;
    case LOK_CALLBACK_SEARCH_NOT_FOUND:
      emit({.kind = EngineEventKind::search_not_found, .text = payload});
      break;
    case LOK_CALLBACK_COMMENT:
      emit({.kind = EngineEventKind::comments_changed, .text = payload});
      break;
    case LOK_CALLBACK_REDLINE_TABLE_SIZE_CHANGED:
    case LOK_CALLBACK_REDLINE_TABLE_ENTRY_MODIFIED:
      emit({.kind = EngineEventKind::tracked_changes_changed, .text = payload});
      break;
    case LOK_CALLBACK_TABLE_SELECTED:
      emit({.kind = EngineEventKind::table_selection_changed, .text = payload});
      break;
    case LOK_CALLBACK_FONTS_MISSING: {
      const auto fonts = quoted_strings(payload);
      {
        std::lock_guard lock(state_mutex);
        document_status.missing_fonts = fonts;
      }
      emit({.kind = EngineEventKind::missing_fonts_changed, .text = payload});
      break;
    }
    case LOK_CALLBACK_A11Y_FOCUS_CHANGED:
    case LOK_CALLBACK_A11Y_CARET_CHANGED:
    case LOK_CALLBACK_A11Y_TEXT_SELECTION_CHANGED:
      emit({.kind = EngineEventKind::accessibility_focus_changed, .text = payload});
      break;
    case LOK_CALLBACK_CONTEXT_MENU:
      emit({.kind = EngineEventKind::context_menu, .text = payload});
      break;
    case LOK_CALLBACK_WINDOW: {
      const auto id = json_string_value(payload, "id");
      if (id.has_value()) {
        last_window_id = std::atoi(id->c_str());
      }
      // LibreOffice 26.2 uses a legacy Yes/No dialog for removal. Its
      // synchronous dispatch waits here, so forwarding it to GTK's idle
      // queue would deadlock the owner thread. The explicit removal command
      // already supplies consent; answer only that particular dialog through
      // the supported window event API. The legacy dialog's affirmative
      // button is the trailing button in its bottom action row.
      const auto unique_id = json_string_value(payload, "unique_id");
      const auto action = json_string_value(payload, "action");
      std::string expected;
      {
        std::lock_guard lock(state_mutex);
        expected = pending_page_part_removal;
      }
      if (document && id.has_value() && action == "created" &&
          !expected.empty() && unique_id == expected) {
        const auto window_id = static_cast<unsigned>(std::stoul(*id));
        const auto size = json_string_value(payload, "size").value_or("");
        const auto comma = size.find(',');
        if (comma != std::string::npos) {
          const int x = std::stoi(size.substr(0, comma)) - 50;
          const int y = std::stoi(size.substr(comma + 1)) - 20;
          document->postWindowMouseEvent(window_id, LOK_MOUSEEVENT_MOUSEBUTTONDOWN, x, y, 1, 1, 0);
          document->postWindowMouseEvent(window_id, LOK_MOUSEEVENT_MOUSEBUTTONUP, x, y, 1, 1, 0);
        }
      }
      if (payload.find("\"action\": \"close\"") != std::string::npos ||
          payload.find("\"action\":\"close\"") != std::string::npos) {
        dialog_open = false;
      }
      break;
    }
    case LOK_CALLBACK_JSDIALOG: {
      // The first message for a dialog carries its whole widget tree; later
      // ones are updates to controls in it.
      const auto kind = json_string_value(payload, "type");
      const auto name = json_string_value(payload, "id");
      if (kind.has_value() && *kind == "dialog" && name.has_value() && !dialog_open) {
        dialog_open = true;
        emit({.kind = EngineEventKind::dialog_opened, .text = *name,
              .numeric_value = last_window_id});
      }
      break;
    }
    case LOK_CALLBACK_HYPERLINK_CLICKED:
      emit({.kind = EngineEventKind::hyperlink_activated, .text = payload});
      break;
    case LOK_CALLBACK_ERROR:
      emit({.kind = EngineEventKind::error, .text = payload});
      break;
    default:
      break;
  }
}

LibreOfficeEngine::LibreOfficeEngine() : impl_(std::make_unique<Impl>()) {}
LibreOfficeEngine::~LibreOfficeEngine() { impl_->shutdown(); }

Result LibreOfficeEngine::initialize(const EngineConfiguration& configuration) {
  return impl_->initialize(configuration);
}

std::string LibreOfficeEngine::engine_version() const { return impl_->version; }

void LibreOfficeEngine::set_event_sink(EventSink sink) {
  std::lock_guard lock(impl_->state_mutex);
  impl_->sink = std::move(sink);
}

Result LibreOfficeEngine::create_blank() {
  return impl_->load("private:factory/swriter", std::nullopt);
}

Result LibreOfficeEngine::open(const std::filesystem::path& path) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error) {
    return Result::failure("Could not resolve the document path: " + error.message());
  }
  if (!std::filesystem::is_regular_file(canonical)) {
    return Result::failure("The selected document is not a regular file");
  }
  return impl_->load(file_url(canonical), canonical);
}

void LibreOfficeEngine::close() noexcept {
  if (impl_->owner_thread != std::this_thread::get_id()) {
    return;
  }
  impl_->release_document();
  {
    std::lock_guard lock(impl_->state_mutex);
    impl_->document_status = {};
    impl_->command_states.clear();
  }
}

bool LibreOfficeEngine::has_document() const noexcept { return impl_->document != nullptr; }

DocumentStatus LibreOfficeEngine::status() const {
  std::lock_guard lock(impl_->state_mutex);
  return impl_->document_status;
}

Result LibreOfficeEngine::render_tile(const TileRequest& request, RenderedTile& output) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  if (request.pixel_width <= 0 || request.pixel_height <= 0 ||
      request.pixel_width > kMaximumTileEdge || request.pixel_height > kMaximumTileEdge ||
      request.document_area_twips.empty()) {
    return Result::failure("Invalid or excessively large tile request");
  }
  const auto width = static_cast<std::size_t>(request.pixel_width);
  const auto height = static_cast<std::size_t>(request.pixel_height);
  if (width > std::numeric_limits<std::size_t>::max() / height / kPixelBytes) {
    return Result::failure("Tile buffer size overflow");
  }
  output = {};
  output.request = request;
  output.stride = width * kPixelBytes;
  output.pixels.resize(output.stride * height);
  output.format = impl_->document->getTileMode() == LOK_TILEMODE_RGBA ? PixelFormat::rgba8
                                                                     : PixelFormat::bgra8;
  impl_->document->paintTile(
      reinterpret_cast<unsigned char*>(output.pixels.data()), request.pixel_width,
      request.pixel_height, checked_int(request.document_area_twips.x),
      checked_int(request.document_area_twips.y),
      checked_int(request.document_area_twips.width),
      checked_int(request.document_area_twips.height));
  return Result::success();
}

Result LibreOfficeEngine::set_visible_area(const Rect& area_twips) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  impl_->document->setClientVisibleArea(checked_int(area_twips.x), checked_int(area_twips.y),
                                        checked_int(area_twips.width),
                                        checked_int(area_twips.height));
  return Result::success();
}

Result LibreOfficeEngine::send_key(const KeyEvent& event) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  constexpr std::uint32_t kShiftModifier = 0x1000U;
  constexpr std::uint32_t kControlModifier = 0x2000U;
  constexpr std::uint32_t kAltModifier = 0x4000U;
  constexpr std::uint32_t kMetaModifier = 0x8000U;
  constexpr std::uint32_t kKeyCodeMask = 0x0FFFU;
  const auto key_code = event.platform_independent_key & kKeyCodeMask;
  const auto modifiers = event.platform_independent_key & ~kKeyCodeMask;
  if (event.phase == KeyPhase::released) {
    return Result::success();
  }

  // The application owns Alt/Meta chords. Control is accepted only by the
  // explicit navigation map below; all other Control chords remain owned by
  // the GTK action/shortcut layer.
  if ((modifiers & (kAltModifier | kMetaModifier)) != 0U) {
    return Result::success();
  }

  const bool selecting = (modifiers & kShiftModifier) != 0U;
  const bool by_unit = (modifiers & kControlModifier) != 0U;
  const char* navigation_command = nullptr;
  switch (key_code) {
    case 1024U:
      navigation_command =
          by_unit ? (selecting ? ".uno:EndOfParaSel" : ".uno:GoToNextPara")
                  : (selecting ? ".uno:LineDownSel" : ".uno:GoDown");
      break;
    case 1025U:
      navigation_command =
          by_unit ? (selecting ? ".uno:StartOfParaSel" : ".uno:GoToStartOfPara")
                  : (selecting ? ".uno:LineUpSel" : ".uno:GoUp");
      break;
    case 1026U:
      navigation_command =
          by_unit ? (selecting ? ".uno:WordLeftSel" : ".uno:GoToPrevWord")
                  : (selecting ? ".uno:CharLeftSel" : ".uno:GoLeft");
      break;
    case 1027U:
      navigation_command =
          by_unit ? (selecting ? ".uno:WordRightSel" : ".uno:GoToNextWord")
                  : (selecting ? ".uno:CharRightSel" : ".uno:GoRight");
      break;
    case 1028U:
      navigation_command =
          by_unit ? (selecting ? ".uno:StartOfDocumentSel" : ".uno:GoToStartOfDoc")
                  : (selecting ? ".uno:StartOfLineSel" : ".uno:GoToStartOfLine");
      break;
    case 1029U:
      navigation_command =
          by_unit ? (selecting ? ".uno:EndOfDocumentSel" : ".uno:GoToEndOfDoc")
                  : (selecting ? ".uno:EndOfLineSel" : ".uno:GoToEndOfLine");
      break;
    case 1030U:
      if (!by_unit) {
        navigation_command = selecting ? ".uno:PageUpSel" : ".uno:PageUp";
      }
      break;
    case 1031U:
      if (!by_unit) {
        navigation_command = selecting ? ".uno:PageDownSel" : ".uno:PageDown";
      }
      break;
    default:
      break;
  }
  if (navigation_command != nullptr) {
    // Raw LOK key events are UI-queued. Notified UNO dispatch is model-
    // synchronous in the qualified 26.2 engine, preserving Arrow-then-type
    // ordering without waiting for its separately queued result callback.
    impl_->document->postUnoCommand(navigation_command, nullptr, true);
    return Result::success();
  }
  if ((modifiers & kControlModifier) != 0U) {
    return Result::success();
  }

  if (key_code == 1283U || key_code == 1286U) {
    if ((modifiers & kShiftModifier) != 0U) {
      return Result::success();
    }
    if (impl_->graphic_selected.load()) {
      impl_->mark_command_mutation(true);
      impl_->document->postUnoCommand(".uno:Delete", nullptr, true);
      return Result::success();
    }
    // With window id 0, LibreOfficeKit explicitly executes Backspace/Delete
    // synchronously in the document window. A boundary no-op is conservatively
    // considered dirty until the next successful Save.
    impl_->document->removeTextContext(0, key_code == 1283U ? 1 : 0,
                                       key_code == 1286U ? 1 : 0);
    impl_->mark_modified();
    return Result::success();
  }

  if (key_code == 1280U) {
    impl_->mark_command_mutation(true);
    impl_->document->postUnoCommand(
        (modifiers & kShiftModifier) != 0U ? ".uno:InsertLinebreak"
                                           : ".uno:InsertPara",
        nullptr, true);
    return Result::success();
  }
  if (key_code == 1282U) {
    if ((modifiers & kShiftModifier) != 0U) {
      return Result::success();
    }
    return impl_->insert_text("\t");
  }
  if (key_code == 1284U) {
    return impl_->insert_text(" ");
  }
  if (event.unicode_scalar >= 0x20U) {
    const auto text = utf8_from_scalar(event.unicode_scalar);
    if (!text.has_value()) {
      return Result::failure("Key input is not a valid Unicode scalar");
    }
    return impl_->insert_text(*text);
  }

  if (key_code == 1281U) {
    impl_->document->postUnoCommand(".uno:Escape", nullptr, true);
  }
  // Insert-mode toggling is intentionally unavailable until it can be exposed
  // and tested as visible application state; never send a hidden raw key.
  return Result::success();
}

Result LibreOfficeEngine::send_composition(const CompositionEvent& event) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  if (event.phase != CompositionPhase::commit) {
    // GTK owns the preedit/candidate lifecycle. Only committed text enters the
    // document model, through a notified command with an exact completion.
    return Result::success();
  }
  return impl_->insert_text(event.utf8);
}

Result LibreOfficeEngine::send_pointer(const PointerEvent& event) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  // Writer's mouse API performs object hit-testing, caret placement and drag
  // selection. setTextSelection only edits text and leaves a selected picture
  // active, so it cannot implement clicking a picture or leaving its tools.
  const int type = event.phase == PointerPhase::press ? LOK_MOUSEEVENT_MOUSEBUTTONDOWN
      : event.phase == PointerPhase::release ? LOK_MOUSEEVENT_MOUSEBUTTONUP
                                            : LOK_MOUSEEVENT_MOUSEMOVE;
  impl_->document->postMouseEvent(type, checked_int(event.x_twips),
      checked_int(event.y_twips), event.click_count,
      static_cast<int>(event.buttons), static_cast<int>(event.modifiers));
  return Result::success();
}

Result LibreOfficeEngine::execute(const CommandInvocation& invocation) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  const auto mapping = adapter::command_mapping(invocation.command);
  if (mapping.engine_name.empty()) {
    return Result::failure("The command has no engine mapping");
  }
  impl_->mark_command_mutation(command_mutates_document(invocation.command));
  // bNotifyWhenFinished selects synchronous dispatch in the supported 26.2
  // engine. LOK may deliver the diagnostic result callback later.
  impl_->document->postUnoCommand(mapping.engine_name.data(),
                                  invocation.arguments.empty() ? nullptr
                                                               : invocation.arguments.c_str(),
                                  true);
  return Result::success();
}

CommandState LibreOfficeEngine::command_state(const EngineCommand command) const {
  const auto name = adapter::command_mapping(command).engine_name;
  std::lock_guard lock(impl_->state_mutex);
  const auto state = impl_->command_states.find(std::string(name));
  return state == impl_->command_states.end() ? CommandState{} : state->second;
}

Result LibreOfficeEngine::paste(const std::string_view mime_type,
                                const std::span<const std::byte> bytes) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  if (bytes.size() > kMaximumPlainTextClipboardBytes) {
    return Result::failure("Clipboard content exceeds the 64 MiB safety limit");
  }
  if (mime_type == "text/html") {
    // Rich text keeps its formatting and embedded (data:) pictures; the
    // canvas has already fetched remote pictures without blocking.
    if (bytes.empty()) {
      return Result::success();
    }
    impl_->mark_command_mutation(true);
    const bool pasted = impl_->document->paste("text/html",
                                               reinterpret_cast<const char*>(bytes.data()),
                                               bytes.size());
    return pasted ? Result::success() : Result::failure("The engine could not paste this");
  }
  if (mime_type != "text/plain" &&
      mime_type != "text/plain;charset=utf-8") {
    return Result::failure(
        "This MVP accepts only plain UTF-8 clipboard text");
  }
  if (bytes.empty()) {
    return Result::success();
  }
  const std::string text(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size());
  return impl_->insert_text(text);
}

Result LibreOfficeEngine::insert_image(const std::filesystem::path& file) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(file, error)) {
    return Result::failure("The image could not be read");
  }
  // Embedded, not linked; the engine fits it to the text width. Requesting a
  // result makes the dispatch synchronous, so the picture is in the document
  // (and selected) before it is set in line with the text.
  const std::string url = file_url(file);
  std::string escaped;
  for (const char character : url) {
    if (character == '"' || character == '\\') escaped.push_back('\\');
    escaped.push_back(character);
  }
  const std::string arguments = "{\"FileName\":{\"type\":\"string\",\"value\":\"" + escaped +
                                "\"},\"AsLink\":{\"type\":\"boolean\",\"value\":false}}";
  impl_->mark_command_mutation(true);
  impl_->document->postUnoCommand(".uno:InsertGraphic", arguments.c_str(), true);
  impl_->document->postUnoCommand(".uno:SetAnchorToChar", nullptr, true);
  return Result::success();
}

Result LibreOfficeEngine::drag_graphic_handle(const bool start, const std::int64_t x_twips,
                                              const std::int64_t y_twips) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  if (!start) {
    impl_->mark_command_mutation(true);
  }
  impl_->document->setGraphicSelection(start ? LOK_SETGRAPHICSELECTION_START
                                             : LOK_SETGRAPHICSELECTION_END,
                                       checked_int(x_twips), checked_int(y_twips));
  return Result::success();
}

std::string LibreOfficeEngine::selected_text() const {
  if (!impl_->document || impl_->owner_thread != std::this_thread::get_id()) {
    return {};
  }
  char* used_mime_type = nullptr;
  char* text = impl_->document->getTextSelection("text/plain;charset=utf-8", &used_mime_type);
  std::string result = text == nullptr ? std::string{} : std::string(text);
  if (text != nullptr) {
    impl_->office->freeError(text);
  }
  if (used_mime_type != nullptr) {
    impl_->office->freeError(used_mime_type);
  }
  return result;
}

Result LibreOfficeEngine::save_as(const SaveRequest& request) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  if (request.format == DocumentFormat::unknown || request.format == DocumentFormat::pdf) {
    return Result::failure("Save requires an editable document format");
  }
  const std::string extension(format_extension(request.format));
  const std::string destination_url = file_url(request.destination);
  const char* options = request.take_ownership ? "TakeOwnership" : nullptr;
  if (!impl_->document->saveAs(destination_url.c_str(), extension.c_str(), options)) {
    return Result::failure(impl_->last_error("The document engine could not save the file"));
  }
  if (request.take_ownership) {
    return adopt_saved_document(request.destination, request.format);
  }
  return Result::success();
}

Result LibreOfficeEngine::validate_saved_document(const std::filesystem::path& path) {
  if (const auto thread_result = impl_->require_owner_thread(); !thread_result.ok) {
    return thread_result;
  }
  if (!impl_->office) {
    return Result::failure("The document engine has not been initialized");
  }
  const int live_view = impl_->document ? impl_->document->getView() : -1;
  const std::string url = file_url(path);
  Result validation_result = Result::success();
  {
    std::unique_ptr<lok::Document> validation(
        impl_->office->documentLoad(url.c_str(),
                                    "MacroExecutionMode=0,UpdateDocMode=0"));
    if (!validation) {
      validation_result = Result::failure(
          impl_->last_error("Saved output could not be reopened"));
    } else if (validation->getDocumentType() != LOK_DOCTYPE_TEXT) {
      validation_result =
          Result::failure("Saved output reopened as the wrong document type");
    } else {
      long width = 0;
      long height = 0;
      validation->getDocumentSize(&width, &height);
      if (width <= 0 || height <= 0) {
        validation_result = Result::failure(
            "Saved output reopened without a valid layout extent");
      }
    }
  }
  if (impl_->document && live_view >= 0) {
    impl_->document->setView(live_view);
  }
  return validation_result;
}

Result LibreOfficeEngine::adopt_saved_document(
    const std::filesystem::path& path, const DocumentFormat format) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  if (format != DocumentFormat::docx && format != DocumentFormat::odt) {
    return Result::failure(
        "Only validated DOCX or ODT output can become the editable document");
  }
  std::error_code error;
  auto adopted_path = std::filesystem::weakly_canonical(path, error);
  if (error) {
    error.clear();
    adopted_path = std::filesystem::absolute(path, error);
  }
  if (error || !std::filesystem::is_regular_file(adopted_path, error) || error) {
    return Result::failure("The committed document path could not be adopted");
  }
  std::lock_guard lock(impl_->state_mutex);
  impl_->document_status.identity.original_path = adopted_path;
  impl_->document_status.identity.extension =
      std::string{"."} + std::string{format_extension(format)};
  impl_->document_status.identity.save_format = format;
  impl_->document_status.identity.mime_type =
      std::string(format_mime_type(format));
  impl_->document_status.modified = false;
  impl_->document_status.source_writable = source_path_writable(adopted_path);
  impl_->document_status.last_explicit_save =
      std::chrono::system_clock::now();
  return Result::success();
}

Result LibreOfficeEngine::export_pdf(const std::filesystem::path& destination) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  const std::string destination_url = file_url(destination);
  if (!impl_->document->saveAs(destination_url.c_str(), "pdf", nullptr)) {
    return Result::failure(impl_->last_error("PDF export failed"));
  }
  return Result::success();
}

AccessibilitySnapshot LibreOfficeEngine::accessibility_snapshot() const {
  AccessibilitySnapshot snapshot;
  if (!impl_->document || impl_->owner_thread != std::this_thread::get_id()) {
    snapshot.editable = false;
    return snapshot;
  }
  if (char* paragraph = impl_->document->getA11yFocusedParagraph(); paragraph != nullptr) {
    snapshot.focused_paragraph_utf8 = paragraph;
    impl_->office->freeError(paragraph);
  }
  snapshot.caret_offset = impl_->document->getA11yCaretPosition();
  const auto status_copy = status();
  snapshot.editable = !status_copy.read_only;
  snapshot.page_count = status_copy.page_count;
  return snapshot;
}

Result LibreOfficeEngine::dispatch_command(const std::string_view uno_command,
                                           const std::string_view json_arguments,
                                           const bool modifies_document) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  if (!uno_command.starts_with(".uno:")) {
    return Result::failure("Not an engine command");
  }
  if (!json_arguments.empty() && !valid_utf8(json_arguments)) {
    return Result::failure("Command arguments are not valid UTF-8");
  }
  impl_->mark_command_mutation(modifies_document);
  const std::string command(uno_command);
  const std::string arguments(json_arguments);
  {
    std::lock_guard lock(impl_->state_mutex);
    if (command == ".uno:InsertPageHeader" || command == ".uno:InsertPageFooter")
      impl_->pending_page_part_removal = command == ".uno:InsertPageHeader"
          ? "DeleteHeaderDialog" : "DeleteFooterDialog";
  }
  impl_->document->postUnoCommand(command.c_str(),
                                  arguments.empty() ? nullptr : arguments.c_str(),
                                  true);
  {
    std::lock_guard lock(impl_->state_mutex);
    impl_->pending_page_part_removal.clear();
  }
  return Result::success();
}

std::optional<std::string> LibreOfficeEngine::command_value(
    const std::string_view uno_command) const {
  std::lock_guard lock(impl_->state_mutex);
  const auto found = impl_->raw_states.find(std::string(uno_command));
  if (found == impl_->raw_states.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::string LibreOfficeEngine::command_values(const std::string_view uno_command) {
  if (!impl_->document || impl_->owner_thread != std::this_thread::get_id()) {
    return {};
  }
  char* values = impl_->document->getCommandValues(std::string(uno_command).c_str());
  std::string result = values == nullptr ? std::string{} : std::string(values);
  if (values != nullptr) {
    impl_->office->freeError(values);
  }
  return result;
}

Result LibreOfficeEngine::set_view_theme(const ViewTheme theme) {
  if (const auto thread_result = impl_->require_owner_thread(); !thread_result.ok) {
    return thread_result;
  }
  impl_->view_theme = theme;
  impl_->apply_view_theme();
  return Result::success();
}

std::vector<OutlineHeading> LibreOfficeEngine::outline() {
  if (!impl_->document || impl_->owner_thread != std::this_thread::get_id() ||
      !impl_->configuration.private_profile_directory.has_value()) {
    return {};
  }
  // A flat-XML copy in the private profile: the only complete, ordered view
  // of the headings LibreOfficeKit offers. The copy does not change the
  // document's location, modified state or undo history.
  const auto copy = *impl_->configuration.private_profile_directory / "outline.fodt";
  impl_->quiet_progress = true;
  const std::string url = file_url(copy);
  const bool saved = impl_->document->saveAs(url.c_str(), "fodt", nullptr);
  impl_->quiet_progress = false;
  if (!saved) {
    return {};
  }
  std::ifstream stream(copy, std::ios::binary);
  const std::string xml((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  std::error_code ignored;
  std::filesystem::remove(copy, ignored);
  return headings_from_flat_odt(xml);
}

Result LibreOfficeEngine::answer_dialog(const int window_id, const bool confirm) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  const char* event = confirm
      ? "{\"id\":\"ok\",\"cmd\":\"click\",\"type\":\"responsebutton\",\"data\":\"\"}"
      : "{\"id\":\"cancel\",\"cmd\":\"click\",\"type\":\"responsebutton\",\"data\":\"\"}";
  impl_->document->sendDialogEvent(static_cast<unsigned long long>(window_id), event);
  impl_->dialog_open = false;
  if (confirm) {
    impl_->mark_modified();
  }
  return Result::success();
}

Result LibreOfficeEngine::request_context_menu(const std::int64_t x_twips,
                                               const std::int64_t y_twips) {
  if (const auto ready = impl_->require_document(); !ready.ok) {
    return ready;
  }
  constexpr int kRightButton = 4;
  impl_->document->postMouseEvent(LOK_MOUSEEVENT_MOUSEBUTTONDOWN, checked_int(x_twips),
                                  checked_int(y_twips), 1, kRightButton, 0);
  impl_->document->postMouseEvent(LOK_MOUSEEVENT_MOUSEBUTTONUP, checked_int(x_twips),
                                  checked_int(y_twips), 1, kRightButton, 0);
  return Result::success();
}

}  // namespace luma::write::engine
