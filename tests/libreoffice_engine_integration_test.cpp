#include "engine/libreoffice_engine.hpp"
#include "persistence/atomic_save.hpp"
#include <glib.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

using luma::write::engine::CommandInvocation;
using luma::write::engine::CompositionEvent;
using luma::write::engine::CompositionPhase;
using luma::write::engine::DocumentFormat;
using luma::write::engine::EngineCommand;
using luma::write::engine::EngineConfiguration;
using luma::write::engine::KeyEvent;
using luma::write::engine::KeyPhase;
using luma::write::engine::LibreOfficeEngine;
using luma::write::engine::Rect;
using luma::write::engine::SaveRequest;
using luma::write::engine::TileRequest;
using luma::write::engine::RenderedTile;
using luma::write::persistence::AtomicFileSaver;
using luma::write::persistence::SaveOutcome;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "libreoffice_engine_integration_test: " << message << '\n';
  std::exit(1);
}

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    fail(message);
  }
}

void expect_ok(const luma::write::engine::Result& result,
               const std::string& operation) {
  if (!result.ok) {
    fail(operation + ": " + result.message);
  }
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count();
    path_ = std::filesystem::temp_directory_path() /
            ("luma-write-lok-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

void expect_nonempty_file(const std::filesystem::path& path,
                          const std::string& label) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  expect(!error && size > 0, label + " was not written");
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{stream},
          std::istreambuf_iterator<char>{}};
}

void settle_engine() {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{150};
  while (std::chrono::steady_clock::now() < until) {
    while (g_main_context_iteration(nullptr, FALSE)) {}
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  TemporaryDirectory temporary;
  LibreOfficeEngine engine;
  expect_ok(engine.initialize(EngineConfiguration{
                .private_profile_directory = temporary.path() / "profile",
                .disable_active_content = true,
                .enable_accessibility = true,
            }),
            "initialize Fedora LibreOfficeKit");
  expect(!engine.engine_version().empty(), "engine version was not reported");

  constexpr std::string_view kRoundTripText =
      "Project Luma real LibreOfficeKit round trip";
  const std::string scenario = argc > 1 ? argv[1] : "core";
  if (scenario == "images") {
    using namespace luma::write::engine;
    std::optional<Rect> picture;
    engine.set_event_sink([&](const EngineEvent& event) {
      if (event.kind == EngineEventKind::graphic_selection_changed) picture = event.area_twips;
    });
    expect_ok(engine.create_blank(), "create image fixture");
    RenderedTile rendered;
    expect_ok(engine.render_tile({.document_area_twips = {0, 0, 12000, 15000},
        .pixel_width = 400, .pixel_height = 500}, rendered), "paint image fixture");
    const auto image = temporary.path() / "fixture.png";
    gsize count = 0;
    guchar* bytes = g_base64_decode("iVBORw0KGgoAAAANSUhEUgAAAZAAAADICAIAAABJdyC1AAACq0lEQVR4nO3UQQ0AIBDAsJOIJCQhEQv8yJImFbDXZu0DkDDfCwAeGRaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQIZhARmGBWQYFpBhWECGYQEZhgVkGBaQYVhAhmEBGYYFZBgWkGFYQMYFu8fbbZAhUVcAAAAASUVORK5CYII=", &count);
    expect(g_file_set_contents(image.c_str(), reinterpret_cast<const char*>(bytes),
        static_cast<gssize>(count), nullptr), "write generated image fixture");
    g_free(bytes);
    expect_ok(engine.insert_image(image), "insert embedded image");
    settle_engine();
    expect(picture.has_value(), "Inserted picture was not selected");
    const Rect original = *picture;
    const auto click = [&](std::int64_t x, std::int64_t y) {
      expect_ok(engine.send_pointer({.phase = PointerPhase::press, .x_twips = x,
          .y_twips = y, .buttons = 1}), "mouse press");
      expect_ok(engine.send_pointer({.phase = PointerPhase::release, .x_twips = x,
          .y_twips = y, .buttons = 1}), "mouse release");
      settle_engine();
    };
    click(original.x + original.width + 600, original.y + original.height + 600);
    expect(!picture.has_value(), "Clicking outside the image did not leave image selection");
    expect_ok(engine.send_composition({.phase = CompositionPhase::commit,
        .utf8 = "Text after picture"}), "type without clicking Done");
    settle_engine();
    click(original.x + original.width / 2, original.y + original.height / 2);
    expect(picture.has_value(), "A single click did not reselect the image");
    expect_ok(engine.dispatch_command(".uno:SetAnchorToPara", "", true), "float image");
    expect_ok(engine.dispatch_command(".uno:WrapIdeal", "", true), "wrap image");
    settle_engine();
    const Rect before_move = *picture;
    const auto cx = before_move.x + before_move.width / 2;
    const auto cy = before_move.y + before_move.height / 2;
    expect_ok(engine.send_pointer({.phase = PointerPhase::press, .x_twips = cx,
        .y_twips = cy, .buttons = 1}), "start image move");
    settle_engine();
    for (int step = 1; step <= 8; ++step) {
      expect_ok(engine.send_pointer({.phase = PointerPhase::move, .x_twips = cx + step * 100,
          .y_twips = cy + step * 75, .buttons = 1}), "drag image");
      settle_engine();
    }
    expect_ok(engine.send_pointer({.phase = PointerPhase::release, .x_twips = cx + 800,
        .y_twips = cy + 600, .buttons = 1}), "finish image move");
    settle_engine();
    expect(picture.has_value() && (picture->x != before_move.x || picture->y != before_move.y),
        "Dragging the picture did not move it");
    const Rect before_resize = *picture;
    expect_ok(engine.drag_graphic_handle(true, before_resize.x + before_resize.width,
        before_resize.y + before_resize.height), "start corner resize");
    expect_ok(engine.drag_graphic_handle(false, before_resize.x + before_resize.width * 3 / 4,
        before_resize.y + before_resize.height * 3 / 4), "finish corner resize");
    settle_engine();
    expect(picture.has_value() && picture->width < before_resize.width &&
        picture->height < before_resize.height, "Corner handle did not resize the image");
    const Rect resized = *picture;
    expect_ok(engine.dispatch_command(".uno:Escape", "", false), "leave image with Escape");
    settle_engine();
    expect(!picture.has_value(), "Escape did not leave image selection");
    const auto document = temporary.path() / "images.docx";
    expect_ok(engine.save_as({.destination = document, .format = DocumentFormat::docx}),
        "save moved and resized image");
    expect_ok(engine.open(document), "reopen image document");
    expect_ok(engine.render_tile({.document_area_twips = {0, 0, 12000, 15000},
        .pixel_width = 400, .pixel_height = 500}, rendered), "paint reopened image");
    settle_engine();
    click(resized.x + resized.width / 2, resized.y + resized.height / 2);
    expect(picture.has_value() && std::abs(picture->width - resized.width) < 30 &&
        std::abs(picture->height - resized.height) < 30 &&
        std::abs(picture->x - resized.x) < 30 && std::abs(picture->y - resized.y) < 30,
        "Image position or dimensions did not survive DOCX save and reopen");
    expect_ok(engine.send_key({.phase = KeyPhase::pressed, .platform_independent_key = 1286}),
        "delete selected image with keyboard");
    settle_engine();
    expect(!picture.has_value(), "Delete key did not remove the selected image");
    expect_ok(engine.execute({.command = EngineCommand::undo}), "undo image deletion");
    settle_engine();
    expect_ok(engine.dispatch_command(".uno:Escape", "", false), "return to text after Undo");
    expect_ok(engine.execute({.command = EngineCommand::select_all}), "select reopened text");
    expect(engine.selected_text().find("Text after picture") != std::string::npos,
        "Text typed after leaving image selection did not survive save and reopen");
    engine.set_event_sink({});
    std::cout << "Image insert, click away, single-click selection, drag, resize, Escape and DOCX round trip pass\n";
    return 0;
  }
  if (scenario == "import-audit" || scenario == "import-audit-verify") {
    expect(argc >= 4, "import-audit requires an input file and an output directory");
    const std::filesystem::path input{argv[2]};
    const std::filesystem::path output{argv[3]};
    std::filesystem::create_directories(output);
    expect_ok(engine.open(input), "open external legacy document");
    if (scenario == "import-audit-verify") {
      expect_ok(engine.save_as(SaveRequest{.destination = output / "after.odt",
          .format = DocumentFormat::odt}), "capture reopened structure");
      expect_ok(engine.export_pdf(output / "after.pdf"), "render reopened document");
      std::cout << "Reopened pages: " << engine.status().page_count << std::endl;
      return 0;
    }
    const auto imported = engine.status();
    RenderedTile first_tile;
    expect_ok(engine.render_tile(TileRequest{.document_area_twips = Rect{0, 0, 6000, 6000},
        .pixel_width = 64, .pixel_height = 64, .revision = imported.revision}, first_tile),
        "paint imported document");
    settle_engine();
    std::cout << "Imported pages: " << engine.status().page_count << '\n';
    for (const char* command : {".uno:PageStyleName", ".uno:InsertPageHeader", ".uno:InsertPageFooter",
                               ".uno:LineSpacing", ".uno:ULSpacing", ".uno:StateWordCount"})
      std::cout << command << ": " << engine.command_value(command).value_or("unavailable") << '\n';
    expect_ok(engine.save_as(SaveRequest{.destination = output / "before.odt",
        .format = DocumentFormat::odt}), "capture imported structure");
    expect_ok(engine.export_pdf(output / "before.pdf"), "render imported document");
    expect_ok(engine.dispatch_command(".uno:GoToEndOfDoc", "", false), "move to end of imported document");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.send_composition(CompositionEvent{.phase = CompositionPhase::commit,
        .utf8 = " LUMA_IMPORT_AUDIT"}), "edit imported document");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const bool preserve_odt = argc > 4 && std::string_view{argv[4]} == "odt";
    const auto migrated = output / (preserve_odt ? "edited.odt" : "edited.docx");
    expect_ok(engine.save_as(SaveRequest{.destination = migrated,
        .format = preserve_odt ? DocumentFormat::odt : DocumentFormat::docx}), "save edited legacy import");
    std::ofstream(output / "annotations.json") << engine.command_values(".uno:ViewAnnotations");
    const std::string migrated_path = migrated.string();
    const std::string output_path = output.string();
    gchar* arguments[] = {argv[0], const_cast<gchar*>("import-audit-verify"),
        const_cast<gchar*>(migrated_path.c_str()), const_cast<gchar*>(output_path.c_str()), nullptr};
    gint exit_status = 0;
    GError* error = nullptr;
    const bool reopened = g_spawn_sync(nullptr, arguments, nullptr, G_SPAWN_SEARCH_PATH,
        nullptr, nullptr, nullptr, nullptr, &exit_status, &error);
    g_clear_error(&error);
    expect(reopened && exit_status == 0, "fresh-process reopen failed");
    std::cout << "libreoffice_engine_integration_test (import-audit): PASS\n";
    return 0;
  }
  if (scenario == "format-panel") {
    expect_ok(engine.create_blank(), "create formatting regression document");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.send_composition(CompositionEvent{
        .phase = CompositionPhase::commit, .utf8 = "Format panel regression"}), "type formatting text");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.dispatch_command(".uno:SelectAll", "", false), "select formatting text");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.dispatch_command(".uno:ParaspaceIncrease", "", true), "increase paragraph gap");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.dispatch_command(".uno:ULSpacing",
        "{\"ULSpacing.TopMargin\":{\"type\":\"long\",\"value\":212},"
        "\"ULSpacing.BottomMargin\":{\"type\":\"long\",\"value\":423}}", true), "set paragraph spacing");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.dispatch_command(".uno:LineSpacing",
        "{\"LineSpacing.Mode\":{\"type\":\"short\",\"value\":0},"
        "\"LineSpacing.Height\":{\"type\":\"short\",\"value\":115}}", true), "set custom line spacing");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.dispatch_command(".uno:Color",
        "{\"Color\":{\"type\":\"long\",\"value\":3105481}}", true), "set text colour");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.dispatch_command(".uno:AttributePageSize",
        "{\"AttributePageSize.Width\":{\"type\":\"long\",\"value\":21000},"
        "\"AttributePageSize.Height\":{\"type\":\"long\",\"value\":29700}}", true), "set A4 paper");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.dispatch_command(".uno:ToggleOrientation", "", true), "set landscape orientation");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto output = temporary.path() / "format-panel.odt";
    expect_ok(engine.dispatch_command(".uno:PageLRMargin",
        "{\"Page.Left\":{\"type\":\"long\",\"value\":1000},"
        "\"Page.Right\":{\"type\":\"long\",\"value\":2000}}", true), "set custom horizontal margins");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.dispatch_command(".uno:PageULMargin",
        "{\"Page.Upper\":{\"type\":\"long\",\"value\":1200},"
        "\"Page.Lower\":{\"type\":\"long\",\"value\":1600}}", true), "set custom vertical margins");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    expect_ok(engine.save_as(SaveRequest{.destination = output, .format = DocumentFormat::odt}),
              "save formatting changes");
    for (const char* command : {".uno:ULSpacing", ".uno:LineSpacing", ".uno:Color",
                               ".uno:InsertPageHeader", ".uno:InsertPageFooter", ".uno:PageStyleName"})
      std::cout << command << ": " << engine.command_value(command).value_or("unavailable") << '\n';
    if (const char* audit = std::getenv("LUMA_WRITE_FORMAT_TEST_OUTPUT"))
      std::filesystem::copy_file(output, audit, std::filesystem::copy_options::overwrite_existing);
    const std::string path = output.string();
    const char* inspect = "import sys,zipfile,xml.etree.ElementTree as E; "
        "r=E.fromstring(zipfile.ZipFile(sys.argv[1]).read('content.xml')); "
        "fo='{urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0}'; "
        "a=[e.attrib for e in r.iter()]; "
        "assert any(float(d.get(fo+'margin-bottom','0in')[:-2])>0 for d in a); "
        "assert any(abs(float(d.get(fo+'margin-top','0in')[:-2])-1/12)<0.002 for d in a); "
        "assert any(abs(float(d.get(fo+'margin-bottom','0in')[:-2])-1/6)<0.002 for d in a); "
        "assert any(d.get(fo+'line-height')=='115%' for d in a); "
        "assert any(d.get(fo+'color')=='#2f62c9' for d in a); "
        "s=E.fromstring(zipfile.ZipFile(sys.argv[1]).read('styles.xml')); "
        "p=[e.attrib for e in s.iter() if e.tag.endswith('page-layout-properties')]; "
        "assert any(abs(float(d.get(fo+'page-width','0in')[:-2])-29700/2540)<0.002 "
        "and abs(float(d.get(fo+'page-height','0in')[:-2])-21000/2540)<0.002 for d in p); "
        "assert any(all(abs(float(d.get(fo+'margin-'+k,'0in')[:-2])-v/2540)<0.002 "
        "for k,v in [('left',1000),('right',2000),('top',1200),('bottom',1600)]) for d in p)";
    gchar* arguments[] = {const_cast<gchar*>("python3"), const_cast<gchar*>("-c"),
        const_cast<gchar*>(inspect), const_cast<gchar*>(path.c_str()), nullptr};
    gint exit_status = 0;
    GError* error = nullptr;
    const bool inspected = g_spawn_sync(nullptr, arguments, nullptr, G_SPAWN_SEARCH_PATH,
        nullptr, nullptr, nullptr, nullptr, &exit_status, &error);
    g_clear_error(&error);
    expect(inspected && exit_status == 0, "format controls did not persist their intended values");
    std::cout << "libreoffice_engine_integration_test (format-panel): PASS\n";
    return 0;
  }
  if (scenario == "header-footer") {
    expect_ok(engine.create_blank(), "create header/footer regression document");
    for (const char* command : {".uno:InsertPageHeader", ".uno:InsertPageFooter"}) {
      for (const bool on : {true, false, true, false}) {
        const std::string arguments =
            "{\"PageStyle\":{\"type\":\"string\",\"value\":\"Default Page Style\"},"
            "\"On\":{\"type\":\"boolean\",\"value\":" +
            std::string{on ? "true" : "false"} + "}}";
        expect_ok(engine.dispatch_command(command, arguments, true), "toggle header/footer");
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
      }
    }
    expect_ok(engine.send_composition(CompositionEvent{
        .phase = CompositionPhase::commit, .utf8 = "Still responsive"}), "type after removal");
    const auto output = temporary.path() / "header-footer.odt";
    expect_ok(engine.save_as(SaveRequest{.destination = output, .format = DocumentFormat::odt}),
              "save after header/footer removal");
    expect_ok(engine.validate_saved_document(output), "reopen after header/footer removal");
    const std::string path = output.string();
    const char* inspect = "import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); "
                          "s=z.read('styles.xml'); c=z.read('content.xml'); "
                          "assert b'<style:header>' not in s; "
                          "assert b'<style:footer>' not in s; "
                          "assert b'Still responsive' in c";
    gchar* arguments[] = {const_cast<gchar*>("python3"), const_cast<gchar*>("-c"),
                          const_cast<gchar*>(inspect), const_cast<gchar*>(path.c_str()), nullptr};
    gint exit_status = 0;
    GError* error = nullptr;
    const bool inspected = g_spawn_sync(nullptr, arguments, nullptr, G_SPAWN_SEARCH_PATH,
                                        nullptr, nullptr, nullptr, nullptr, &exit_status, &error);
    g_clear_error(&error);
    expect(inspected && exit_status == 0,
           "header/footer were not removed or subsequent typing was lost");
    if (const char* audit = std::getenv("LUMA_WRITE_HEADER_TEST_OUTPUT"))
      std::filesystem::copy_file(output, audit, std::filesystem::copy_options::overwrite_existing);
    std::cout << "libreoffice_engine_integration_test (header-footer): PASS\n";
    return 0;
  }
  if (scenario != "core") {
    expect_ok(engine.create_blank(), "create blank Writer document");
    expect_ok(engine.send_composition(CompositionEvent{
                  .phase = CompositionPhase::commit,
                  .utf8 = std::string{kRoundTripText},
              }),
              "insert deterministic export text");

    if (scenario == "pdf") {
      const auto output = temporary.path() / "round-trip.pdf";
      expect_ok(engine.export_pdf(output), "export PDF");
      expect_nonempty_file(output, "PDF");
    } else {
      const bool is_odt = scenario == "odt";
      expect(is_odt || scenario == "doc", "unknown integration scenario");
      const auto format = is_odt ? DocumentFormat::odt
                                 : DocumentFormat::legacy_doc;
      const auto output = temporary.path() /
                          (is_odt ? "round-trip.odt" : "round-trip.doc");
      expect_ok(engine.save_as(SaveRequest{
                    .destination = output,
                    .format = format,
                }),
                "save " + scenario);
      expect_nonempty_file(output, scenario);
      expect_ok(engine.validate_saved_document(output),
                "reopen-validate " + scenario);
    }

    std::cout << "libreoffice_engine_integration_test (" << scenario
              << "): PASS\n";
    return 0;
  }

  LibreOfficeEngine refused_second_engine;
  const auto second_initialization = refused_second_engine.initialize(
      EngineConfiguration{
          .private_profile_directory = temporary.path() / "second-profile",
          .disable_active_content = true,
          .enable_accessibility = true,
      });
  expect(!second_initialization.ok,
         "a second owning wrapper was allowed for process-global LibreOfficeKit");

  expect_ok(engine.create_blank(), "create blank Writer document");
  const auto initial_status = engine.status();
  expect(initial_status.extent_twips.width > 0 &&
             initial_status.extent_twips.height > 0,
         "blank document has no renderable extent");

  RenderedTile tile;
  expect_ok(engine.render_tile(
                TileRequest{
                    .document_area_twips =
                        Rect{0, 0, std::min<std::int64_t>(
                                       initial_status.extent_twips.width, 12'000),
                             std::min<std::int64_t>(
                                 initial_status.extent_twips.height, 12'000)},
                    .pixel_width = 256,
                    .pixel_height = 256,
                    .revision = initial_status.revision,
                },
                tile),
            "render first page tile");
  expect(tile.pixels.size() == 256U * 256U * 4U,
         "rendered tile has the wrong bounded buffer size");

  const auto no_op_baseline = temporary.path() / "no-op-baseline.odt";
  expect_ok(engine.save_as(SaveRequest{
                .destination = no_op_baseline,
                .format = DocumentFormat::odt,
            }),
            "save no-op regression baseline");
  expect_ok(engine.adopt_saved_document(no_op_baseline, DocumentFormat::odt),
            "adopt no-op regression baseline");
  expect_ok(engine.send_key(KeyEvent{
                .phase = KeyPhase::pressed,
                .unicode_scalar = 0,
                .platform_independent_key = 1286,
            }),
            "send no-op Delete at the empty-document boundary");
  expect_ok(engine.send_key(KeyEvent{
                .phase = KeyPhase::released,
                .unicode_scalar = 0,
                .platform_independent_key = 1286,
            }),
            "release no-op Delete at the empty-document boundary");
  const auto no_op_saved = temporary.path() / "no-op-after-delete.odt";
  expect_ok(engine.save_as(SaveRequest{
                .destination = no_op_saved,
                .format = DocumentFormat::odt,
            }),
            "save after no-op Delete");
  expect_ok(engine.validate_saved_document(no_op_saved),
            "reopen-validate save after no-op Delete");
  expect_ok(engine.adopt_saved_document(no_op_saved, DocumentFormat::odt),
            "adopt save after no-op Delete");
  expect(!engine.status().modified,
         "a successful save after boundary Delete did not restore clean state");

  expect_ok(engine.send_composition(CompositionEvent{
                .phase = CompositionPhase::commit,
                .utf8 = "AB",
            }),
            "insert navigation ordering baseline");
  expect_ok(engine.send_key(KeyEvent{
                .phase = KeyPhase::pressed,
                .unicode_scalar = 0,
                .platform_independent_key = 1026,
            }),
            "move left before immediate text insertion");
  expect_ok(engine.send_composition(CompositionEvent{
                .phase = CompositionPhase::commit,
                .utf8 = "X",
            }),
            "insert immediately after navigation");
  const auto navigation_text = temporary.path() / "navigation-ordering.txt";
  expect_ok(engine.save_as(SaveRequest{
                .destination = navigation_text,
                .format = DocumentFormat::text,
            }),
            "save navigation ordering result");
  const std::string navigation_result = read_text_file(navigation_text);
  expect(navigation_result.find("AXB") != std::string::npos,
         "Arrow-then-type ordering was not deterministic; text was: " +
             navigation_result);

  expect_ok(engine.execute(CommandInvocation{.command = EngineCommand::bold}),
            "enable bold");
  expect_ok(engine.send_composition(CompositionEvent{
                .phase = CompositionPhase::commit,
                .utf8 = std::string{kRoundTripText},
            }),
            "insert UTF-8 text through composition input");
  const auto round_trip_source_text =
      temporary.path() / "round-trip-source.txt";
  expect_ok(engine.save_as(SaveRequest{
                .destination = round_trip_source_text,
                .format = DocumentFormat::text,
            }),
            "extract live text before package round trip");
  expect(read_text_file(round_trip_source_text).find(kRoundTripText) !=
             std::string::npos,
         "Bold-then-type did not reach the live Writer model");
  const auto docx = temporary.path() / "round-trip.docx";
  {
    std::ofstream(docx, std::ios::binary) << "last known good bytes";
    const auto saved = AtomicFileSaver::save_via_path(
        docx,
        [&engine](const std::filesystem::path& staged, std::string& error) {
          const auto result = engine.save_as(SaveRequest{
              .destination = staged,
              .format = DocumentFormat::docx,
              .take_ownership = false,
          });
          error = result.message;
          return result.ok;
        },
        [&engine](const std::filesystem::path& staged, std::string& error) {
          const auto result = engine.validate_saved_document(staged);
          error = result.message;
          return result.ok;
        });
    expect(saved.saved(), "staged DOCX save failed: " + saved.message);
    expect(saved.outcome == SaveOutcome::Saved,
           "staged DOCX save did not durably replace the destination");
    expect_ok(engine.adopt_saved_document(docx, DocumentFormat::docx),
              "adopt atomically saved DOCX identity");
    const auto saved_status = engine.status();
    expect(!saved_status.modified,
           "validated atomic DOCX save did not mark the logical document clean");
    expect(saved_status.identity.original_path.has_value() &&
               saved_status.identity.original_path->filename() == docx.filename(),
           "validated atomic DOCX save did not update the logical path");
  }
  expect_nonempty_file(docx, "DOCX");
  expect_ok(engine.validate_saved_document(docx), "reopen-validate DOCX");

  expect_ok(engine.execute(CommandInvocation{.command = EngineCommand::undo}),
            "dispatch undo");
  expect_ok(engine.execute(CommandInvocation{.command = EngineCommand::redo}),
            "dispatch redo");

  engine.close();
  expect_ok(engine.open(docx), "reopen saved DOCX");
  const auto reopened_docx_txt = temporary.path() / "reopened-docx.txt";
  expect_ok(engine.save_as(SaveRequest{
                .destination = reopened_docx_txt,
                .format = DocumentFormat::text,
            }),
            "extract text from reopened DOCX");
  const std::string reopened_docx_text = read_text_file(reopened_docx_txt);
  expect(reopened_docx_text.find(kRoundTripText) != std::string::npos,
         "reopened DOCX did not preserve the deterministic edit; text was: " +
             reopened_docx_text);

  std::cout << "libreoffice_engine_integration_test: PASS\n";
  return 0;
}
