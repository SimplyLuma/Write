#include "uno_command_map.hpp"

namespace luma::write::engine::adapter {

CommandMapping command_mapping(const EngineCommand command) noexcept {
  switch (command) {
    case EngineCommand::select_all:
      return {".uno:SelectAll", false};
    case EngineCommand::cut:
      return {".uno:Cut", false};
    case EngineCommand::undo:
      return {".uno:Undo", false};
    case EngineCommand::redo:
      return {".uno:Redo", false};
    case EngineCommand::bold:
      return {".uno:Bold", true};
    case EngineCommand::italic:
      return {".uno:Italic", true};
    case EngineCommand::underline:
      return {".uno:Underline", true};
    case EngineCommand::strikethrough:
      return {".uno:Strikeout", true};
    case EngineCommand::superscript:
      return {".uno:SuperScript", true};
    case EngineCommand::subscript:
      return {".uno:SubScript", true};
    case EngineCommand::align_left:
      return {".uno:LeftPara", true};
    case EngineCommand::align_center:
      return {".uno:CenterPara", true};
    case EngineCommand::align_right:
      return {".uno:RightPara", true};
    case EngineCommand::align_justify:
      return {".uno:JustifyPara", true};
    case EngineCommand::toggle_bullets:
      return {".uno:DefaultBullet", true};
    case EngineCommand::toggle_numbering:
      return {".uno:DefaultNumbering", true};
    case EngineCommand::clear_character_overrides:
      return {".uno:ResetAttributes", false};
    case EngineCommand::clear_paragraph_overrides:
      return {".uno:ParaReset", false};
    case EngineCommand::apply_paragraph_style:
      return {".uno:StyleApply", true};
    case EngineCommand::insert_table:
      return {".uno:InsertTable", false};
    case EngineCommand::insert_image:
      return {".uno:InsertGraphic", false};
    case EngineCommand::insert_page_break:
      return {".uno:InsertPagebreak", false};
    case EngineCommand::find:
    case EngineCommand::replace:
      return {".uno:ExecuteSearch", false};
    case EngineCommand::track_changes:
      return {".uno:TrackChanges", true};
    case EngineCommand::previous_change:
      return {".uno:PreviousTrackedChange", false};
    case EngineCommand::next_change:
      return {".uno:NextTrackedChange", false};
    case EngineCommand::accept_change:
      return {".uno:AcceptTrackedChange", false};
    case EngineCommand::reject_change:
      return {".uno:RejectTrackedChange", false};
    case EngineCommand::add_comment:
      return {".uno:InsertAnnotation", false};
    case EngineCommand::update_table_of_contents:
      return {".uno:UpdateCurIndex", false};
  }
  return {};
}

}  // namespace luma::write::engine::adapter
