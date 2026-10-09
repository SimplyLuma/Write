#pragma once

#include "ui/write_window.hpp"
#include <json-glib/json-glib.h>
#include <libxml/HTMLparser.h>
#include <climits>
#include <cstdlib>

namespace luma::write::ui {

inline std::string comment_initials(const std::string& author) {
  std::string initials;
  bool start = true;
  int count = 0;
  for (const char* cursor = author.c_str(); *cursor != '\0'; cursor = g_utf8_next_char(cursor)) {
    const gunichar character = g_utf8_get_char(cursor);
    if (g_unichar_isspace(character)) start = true;
    else if (start && g_unichar_isalpha(character)) {
      char encoded[7]{};
      g_unichar_to_utf8(g_unichar_toupper(character), encoded);
      initials += encoded;
      start = false;
      if (++count == 2) break;
    }
  }
  return initials.empty() ? "?" : initials;
}

inline std::string comment_plain_text(const std::string& html) {
  if (html.empty() || html.size() > INT_MAX) return {};
  htmlDocPtr document = htmlReadMemory(html.data(), static_cast<int>(html.size()), nullptr, "UTF-8",
      HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
  if (document == nullptr) return {};
  std::string text;
  const auto visit = [&text](const auto& self, xmlNode* node) -> void {
    for (; node != nullptr; node = node->next) {
      if (node->type == XML_TEXT_NODE && node->content != nullptr)
        text += reinterpret_cast<const char*>(node->content);
      else if (node->type == XML_ELEMENT_NODE) {
        const std::string_view name{reinterpret_cast<const char*>(node->name)};
        if (name == "head" || name == "style" || name == "script") continue;
        self(self, node->children);
        if ((name == "p" || name == "div" || name == "br" || name == "li") &&
            !text.empty() && text.back() != '\n') text += '\n';
      }
    }
  };
  visit(visit, xmlDocGetRootElement(document));
  xmlFreeDoc(document);
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::string comment_date_label(const std::string& iso) {
  if (iso.size() < 10) return {};
  GDateTime* now = g_date_time_new_now_local();
  gchar* today = g_date_time_format(now, "%Y-%m-%d");
  const bool same_day = iso.compare(0, 10, today) == 0;
  g_free(today);
  g_date_time_unref(now);
  return same_day && iso.size() >= 16 ? iso.substr(11, 5) : iso.substr(0, 10);
}

inline std::vector<CommentCard> comment_cards_from_json(const std::string& source) {
  std::vector<CommentCard> comments;
  JsonParser* parser = json_parser_new();
  if (!json_parser_load_from_data(parser, source.c_str(), static_cast<gssize>(source.size()), nullptr)) {
    g_object_unref(parser);
    return comments;
  }
  JsonNode* root = json_parser_get_root(parser);
  JsonNode* list_node = JSON_NODE_HOLDS_OBJECT(root)
      ? json_object_get_member(json_node_get_object(root), "comments") : nullptr;
  const auto field = [](JsonObject* item, const char* key) -> std::string {
    JsonNode* node = json_object_get_member(item, key);
    if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node)) return {};
    const auto type = json_node_get_value_type(node);
    if (type == G_TYPE_STRING) return json_node_get_string(node);
    if (type == G_TYPE_INT64) return std::to_string(json_node_get_int(node));
    if (type == G_TYPE_BOOLEAN) return json_node_get_boolean(node) ? "true" : "false";
    return {};
  };
  if (list_node != nullptr && JSON_NODE_HOLDS_ARRAY(list_node)) {
    JsonArray* list = json_node_get_array(list_node);
    for (guint index = 0; index < json_array_get_length(list); ++index) {
      JsonNode* node = json_array_get_element(list, index);
      if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
      JsonObject* item = json_node_get_object(node);
      if (field(item, "resolved") == "true") continue;
      CommentCard comment;
      comment.id = field(item, "id");
      if (comment.id.empty()) continue;
      comment.author = field(item, "author");
      if (comment.author.empty()) comment.author = "Unknown author";
      comment.initials = comment_initials(comment.author);
      comment.time_label = comment_date_label(field(item, "dateTime"));
      comment.text = field(item, "text");
      if (comment.text.empty()) comment.text = comment_plain_text(field(item, "html"));
      const std::string anchor = field(item, "anchorPos");
      const auto comma = anchor.find(',');
      if (comma != std::string::npos) comment.anchor_y = std::strtoll(anchor.c_str() + comma + 1, nullptr, 10);
      comments.push_back(std::move(comment));
    }
  }
  g_object_unref(parser);
  return comments;
}

}  // namespace luma::write::ui
