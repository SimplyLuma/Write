#pragma once

#include <string_view>

namespace luma::commands::ids {

/** Commands whose meaning and placement are shared by every suite app. */
namespace suite {
inline constexpr std::string_view kNewDocument = "suite.file.new";
inline constexpr std::string_view kOpen = "suite.file.open";
inline constexpr std::string_view kSave = "suite.file.save";
inline constexpr std::string_view kSaveAs = "suite.file.save-as";
inline constexpr std::string_view kExportPdf = "suite.file.export-pdf";
inline constexpr std::string_view kUndo = "suite.edit.undo";
inline constexpr std::string_view kRedo = "suite.edit.redo";
inline constexpr std::string_view kCut = "suite.edit.cut";
inline constexpr std::string_view kCopy = "suite.edit.copy";
inline constexpr std::string_view kPaste = "suite.edit.paste";
inline constexpr std::string_view kFind = "suite.edit.find";
inline constexpr std::string_view kCommandPalette = "suite.search.commands";
inline constexpr std::string_view kToggleNavigator = "suite.view.navigator";
inline constexpr std::string_view kToggleInspector = "suite.view.inspector";
inline constexpr std::string_view kZoomIn = "suite.view.zoom-in";
inline constexpr std::string_view kZoomOut = "suite.view.zoom-out";
inline constexpr std::string_view kFitWidth = "suite.view.fit-width";
inline constexpr std::string_view kFitPage = "suite.view.fit-page";
inline constexpr std::string_view kPreferences = "suite.app.preferences";
inline constexpr std::string_view kPrint = "suite.file.print";
inline constexpr std::string_view kSelectAll = "suite.edit.select-all";
}  // namespace suite

/** Stable Write-specific commands; UI labels may change without changing IDs. */
namespace write {
inline constexpr std::string_view kBold = "write.format.bold";
inline constexpr std::string_view kItalic = "write.format.italic";
inline constexpr std::string_view kUnderline = "write.format.underline";
inline constexpr std::string_view kStrikethrough = "write.format.strikethrough";
inline constexpr std::string_view kClearCharacterOverrides =
    "write.format.clear-character-overrides";
inline constexpr std::string_view kClearParagraphOverrides =
    "write.format.clear-paragraph-overrides";
inline constexpr std::string_view kApplyBodyStyle = "write.style.apply-body";
inline constexpr std::string_view kKeepWithNext =
    "write.paragraph.keep-with-next";
inline constexpr std::string_view kInsertTable = "write.insert.table";
inline constexpr std::string_view kInsertImage = "write.insert.image";
inline constexpr std::string_view kInsertPageNumber =
    "write.insert.page-number";
inline constexpr std::string_view kInsertPageBreak =
    "write.insert.page-break";
inline constexpr std::string_view kLandscape =
    "write.page.orientation-landscape";
inline constexpr std::string_view kImageWrap = "write.image.wrap";
inline constexpr std::string_view kImageInline = "write.image.place-inline";
inline constexpr std::string_view kImageAttachParagraph =
    "write.image.attach-paragraph";
inline constexpr std::string_view kImageFixOnPage =
    "write.image.fix-on-page";
inline constexpr std::string_view kAddComment = "write.review.add-comment";
inline constexpr std::string_view kTrackChanges =
    "write.review.track-changes";
inline constexpr std::string_view kNextChange = "write.review.next-change";
inline constexpr std::string_view kPreviousChange = "write.review.previous-change";
inline constexpr std::string_view kAcceptChange = "write.review.accept-change";
inline constexpr std::string_view kRejectChange = "write.review.reject-change";
inline constexpr std::string_view kAcceptAllChanges = "write.review.accept-all";
inline constexpr std::string_view kRejectAllChanges = "write.review.reject-all";

// Commands that carry a value, dispatched with an argument.
inline constexpr std::string_view kApplyStyle = "write.style.apply";
inline constexpr std::string_view kFont = "write.format.font";
inline constexpr std::string_view kBoldState = "write.format.bold-state";
inline constexpr std::string_view kNewStyle = "write.style.new-from-selection";
inline constexpr std::string_view kPictureSize = "write.picture.size";
inline constexpr std::string_view kPictureLayout = "write.picture.layout";
inline constexpr std::string_view kPictureAction = "write.picture.action";
inline constexpr std::string_view kFontSize = "write.format.size";
inline constexpr std::string_view kTextColor = "write.format.text-color";
inline constexpr std::string_view kHighlight = "write.format.highlight";
inline constexpr std::string_view kLineSpacing = "write.paragraph.line-spacing";
inline constexpr std::string_view kParagraphSpacing = "write.paragraph.spacing";
inline constexpr std::string_view kHeaderState = "write.page.header-state";
inline constexpr std::string_view kFooterState = "write.page.footer-state";
inline constexpr std::string_view kInsertCharacter = "write.insert.character";
inline constexpr std::string_view kInsertTableSized = "write.insert.table-sized";
inline constexpr std::string_view kPageSize = "write.page.size";
inline constexpr std::string_view kPageOrientation = "write.page.orientation";
inline constexpr std::string_view kPageMargins = "write.page.margins";
inline constexpr std::string_view kFindNext = "write.find.next";
inline constexpr std::string_view kFindPrevious = "write.find.previous";
inline constexpr std::string_view kReplaceOne = "write.find.replace";
inline constexpr std::string_view kReplaceAll = "write.find.replace-all";
inline constexpr std::string_view kOutlineJump = "write.outline.jump";
inline constexpr std::string_view kZoomTo = "write.view.zoom";
inline constexpr std::string_view kPageTheme = "write.view.page-theme";
inline constexpr std::string_view kRunAdvanced = "write.advanced.run";
inline constexpr std::string_view kReplyComment = "write.comment.reply";
inline constexpr std::string_view kResolveComment = "write.comment.resolve";

// Plain commands.
inline constexpr std::string_view kSuperscript = "write.format.superscript";
inline constexpr std::string_view kSubscript = "write.format.subscript";
inline constexpr std::string_view kGrowFont = "write.format.grow";
inline constexpr std::string_view kShrinkFont = "write.format.shrink";
inline constexpr std::string_view kFormatPainter = "write.format.painter";
inline constexpr std::string_view kChecklist = "write.paragraph.checklist";
inline constexpr std::string_view kIndentMore = "write.paragraph.indent-more";
inline constexpr std::string_view kIndentLess = "write.paragraph.indent-less";
inline constexpr std::string_view kSpaceMore = "write.paragraph.space-more";
inline constexpr std::string_view kSpaceLess = "write.paragraph.space-less";
inline constexpr std::string_view kInsertLink = "write.insert.link";
inline constexpr std::string_view kInsertFootnote = "write.insert.footnote";
inline constexpr std::string_view kInsertContents = "write.insert.contents";
inline constexpr std::string_view kUpdateContents = "write.insert.update-contents";
inline constexpr std::string_view kToggleHeader = "write.page.header";
inline constexpr std::string_view kToggleFooter = "write.page.footer";
inline constexpr std::string_view kReplace = "write.edit.replace";
inline constexpr std::string_view kSpelling = "write.tools.spelling";
inline constexpr std::string_view kFocusMode = "write.view.focus";
inline constexpr std::string_view kAdvancedView = "write.view.advanced";
}  // namespace write

}  // namespace luma::commands::ids
