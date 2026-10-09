#include "commands/command_catalog.h"

#include "commands/command_ids.h"

#include <optional>
#include <string>
#include <utility>

namespace luma::commands {
namespace {

CommandDefinition command(
    const std::string_view id, std::string label, std::string description,
    std::vector<std::string> synonyms,
    std::optional<std::string> shortcut, std::string menu_id,
    const int section_order, const int item_order,
    const CommandContextRequirement context, const bool checkable = false,
    std::optional<std::string> icon_name = std::nullopt) {
  return CommandDefinition{
      .id = std::string{id},
      .label = std::move(label),
      .description = std::move(description),
      .search_synonyms = std::move(synonyms),
      .shortcut = std::move(shortcut),
      .icon_name = std::move(icon_name),
      .menu = {std::move(menu_id), section_order, item_order},
      .context = context,
      .checkable = checkable,
  };
}

}  // namespace

std::vector<CommandDefinition> core_suite_command_definitions() {
  using enum CommandContextRequirement;
  return {
      command(ids::suite::kNewDocument, "New Document",
              "Create a new document in this application.",
              {"blank document", "create file"}, "Primary+N", "file", 0, 0,
              Global, false, "document-new-symbolic"),
      command(ids::suite::kOpen, "Open",
              "Open a local document with the native file chooser.",
              {"open file", "browse documents"}, "Primary+O", "file", 0, 1,
              Global, false, "document-open-symbolic"),
      command(ids::suite::kSave, "Save",
              "Atomically save the current document in its current format.",
              {"save document", "write file"}, "Primary+S", "file", 1, 0,
              Document, false, "document-save-symbolic"),
      command(ids::suite::kSaveAs, "Save As",
              "Choose a path and interoperable format for a document copy.",
              {"save a copy", "change file format"}, "Primary+Shift+S", "file",
              1, 1, Document),
      command(ids::suite::kExportPdf, "Export PDF",
              "Export a non-editable PDF representation of the document.",
              {"make pdf", "publish pdf"}, std::nullopt, "file", 2, 0,
              Document),
      command(ids::suite::kUndo, "Undo", "Undo the most recent operation.",
              {"go back", "reverse last change"}, "Primary+Z", "edit", 0, 0,
              Document, false, "edit-undo-symbolic"),
      command(ids::suite::kRedo, "Redo", "Redo the most recently undone action.",
              {"repeat change", "restore undone change"}, "Primary+Shift+Z",
              "edit", 0, 1, Document, false, "edit-redo-symbolic"),
      command(ids::suite::kCut, "Cut", "Move the current selection to clipboard.",
              {"cut selection", "move to clipboard"}, "Primary+X", "edit", 1,
              0, EditableDocument),
      command(ids::suite::kCopy, "Copy", "Copy the current selection to clipboard.",
              {"copy selection", "clipboard"}, "Primary+C", "edit", 1, 1,
              Document),
      command(ids::suite::kPaste, "Paste",
              "Insert compatible clipboard content at the current location.",
              {"paste clipboard", "insert copied content"}, "Primary+V", "edit",
              1, 2, EditableDocument),
      command(ids::suite::kFind, "Find",
              "Find text or structured content in the current document.",
              {"search document", "find and replace"}, "Primary+F", "edit", 2,
              0, Document, false, "edit-find-symbolic"),
      command(ids::suite::kCommandPalette, "Command Search",
              "Find commands by name, intent, or plain-language synonym.",
              {"command palette", "what can I do", "find a command"},
              "Primary+K", "help", 0, 0, Global, false,
              "system-search-symbolic"),
      command(ids::suite::kToggleNavigator, "Navigator",
              "Show or hide the document-structure panel.",
              {"outline panel", "left sidebar"}, std::nullopt, "view", 0, 0,
              Document, true),
      command(ids::suite::kToggleInspector, "Inspector",
              "Show or hide the contextual properties panel.",
              {"properties panel", "right sidebar", "formatting lens"},
              std::nullopt, "view", 0, 1, Document, true),
      command(ids::suite::kZoomIn, "Zoom In", "Make canvas content appear larger.",
              {"increase zoom", "bigger page"}, "Primary+Plus", "view", 1, 0,
              Document),
      command(ids::suite::kZoomOut, "Zoom Out",
              "Make canvas content appear smaller.",
              {"decrease zoom", "smaller page"}, "Primary+Minus", "view", 1, 1,
              Document),
      command(ids::suite::kFitWidth, "Fit Width",
              "Fit the current page or work area to the canvas width.",
              {"page width", "fill window horizontally"}, std::nullopt, "view", 1,
              2, Document, true),
      command(ids::suite::kFitPage, "Fit Page",
              "Fit one complete page or work area in the canvas.",
              {"whole page", "show full page"}, std::nullopt, "view", 1, 3,
              Document, true),
      command(ids::suite::kPreferences, "Preferences",
              "Configure suite-wide and application preferences.",
              {"settings", "options"}, std::nullopt, "edit", 3, 0, Global),
  };
}

std::vector<CommandDefinition> core_write_command_definitions() {
  using enum CommandContextRequirement;
  return {
      command(ids::write::kBold, "Bold", "Apply or remove bold formatting.",
              {"make text bold", "heavy text"}, "Primary+B", "format", 0, 0,
              TextSelection, true, "format-text-bold-symbolic"),
      command(ids::write::kItalic, "Italic", "Apply or remove italic formatting.",
              {"make text italic", "slanted text"}, "Primary+I", "format", 0, 1,
              TextSelection, true, "format-text-italic-symbolic"),
      command(ids::write::kUnderline, "Underline",
              "Apply or remove underline formatting.",
              {"underline text", "line below text"}, "Primary+U", "format", 0,
              2, TextSelection, true, "format-text-underline-symbolic"),
      command(ids::write::kClearCharacterOverrides,
              "Clear Character Overrides",
              "Remove local character formatting while preserving structure.",
              {"remove weird formatting", "clear text formatting", "reset font"},
              std::nullopt, "format", 1, 0, TextSelection),
      command(ids::write::kClearParagraphOverrides,
              "Clear Paragraph Overrides",
              "Remove local paragraph formatting without removing its style.",
              {"reset paragraph formatting", "remove local spacing"},
              std::nullopt, "format", 1, 1, TextSelection),
      command(ids::write::kApplyBodyStyle, "Apply Body Style",
              "Apply the document's semantic body paragraph style.",
              {"normal text", "body paragraph", "reset to body"}, std::nullopt,
              "format", 2, 0, TextSelection),
      command(ids::write::kKeepWithNext, "Keep With Next",
              "Keep the selected paragraph on the same page as the next one.",
              {"don't split heading", "keep heading with paragraph"},
              std::nullopt, "format", 3, 0, TextSelection, true),
      command(ids::write::kInsertTable, "Table",
              "Insert a table with a chosen number of rows and columns.",
              {"insert grid", "add rows and columns"}, std::nullopt, "insert", 0,
              0, EditableDocument, false, "table-symbolic"),
      command(ids::write::kInsertImage, "Image",
              "Insert an image from a local file or clipboard.",
              {"insert picture", "add photo", "place graphic"}, std::nullopt,
              "insert", 0, 1, EditableDocument, false, "image-x-generic-symbolic"),
      command(ids::write::kInsertPageNumber, "Page Number",
              "Insert a live page-number field in a header or footer.",
              {"number pages", "footer page number"}, std::nullopt, "insert", 1,
              0, EditableDocument),
      command(ids::write::kLandscape, "Landscape",
              "Change the current page or section to landscape orientation.",
              {"horizontal page", "wide page", "page orientation"},
              std::nullopt, "format", 4, 0, EditableDocument),
      command(ids::write::kImageWrap, "Wrap Image",
              "Choose how text flows around the selected image.",
              {"text around image", "picture wrapping", "wrap image"},
              std::nullopt, "format", 5, 0, ImageSelection),
      command(ids::write::kImageInline, "Inline",
              "Make the selected image behave like a character in text.",
              {"image in text", "inline picture"}, std::nullopt, "format", 5, 1,
              ImageSelection, true),
      command(ids::write::kImageAttachParagraph, "Attach to Paragraph",
              "Move the image with an explicitly visible paragraph anchor.",
              {"anchor image", "move picture with paragraph"}, std::nullopt,
              "format", 5, 2, ImageSelection, true),
      command(ids::write::kImageFixOnPage, "Fix on Page",
              "Keep the image at a page-relative location while text reflows.",
              {"lock image to page", "fixed picture"}, std::nullopt, "format", 5,
              3, ImageSelection, true),
      command(ids::write::kAddComment, "Add Comment",
              "Start a review thread anchored to the current selection.",
              {"review note", "comment on selection"}, "Primary+Alt+M", "review",
              0, 0, Document),
      command(ids::write::kTrackChanges, "Track Changes",
              "Record edits as reviewable changes.",
              {"revision mode", "record edits", "redline"}, "Primary+Shift+E",
              "review", 1, 0, EditableDocument, true),
  };
}

std::vector<RegistrationResult> register_write_command_catalog(
    CommandRegistry& registry) {
  std::vector<RegistrationResult> results;
  auto suite_definitions = core_suite_command_definitions();
  auto write_definitions = core_write_command_definitions();
  results.reserve(suite_definitions.size() + write_definitions.size());
  for (auto& definition : suite_definitions) {
    results.push_back(registry.register_command(std::move(definition)));
  }
  for (auto& definition : write_definitions) {
    results.push_back(registry.register_command(std::move(definition)));
  }
  return results;
}

}  // namespace luma::commands
