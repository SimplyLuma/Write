#include "../src/canvas/tile_cache.hpp"
#include "../src/engine/coordinate_space.hpp"
#include "../src/engine/engine_types.hpp"
#include "../src/engine/uno_command_map.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "engine_core_test: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::shared_ptr<luma::write::engine::RenderedTile> make_tile(const std::size_t bytes) {
  auto tile = std::make_shared<luma::write::engine::RenderedTile>();
  tile->pixels.resize(bytes);
  return tile;
}

}  // namespace

int main() {
  using namespace luma::write;

  require(engine::format_extension(engine::DocumentFormat::docx) == "docx",
          "DOCX extension mapping changed");
  require(engine::format_mime_type(engine::DocumentFormat::odt) ==
              "application/vnd.oasis.opendocument.text",
          "ODT MIME mapping changed");
  require(engine::adapter::command_mapping(engine::EngineCommand::cut).engine_name ==
              ".uno:Cut",
          "app-owned Cut command mapping changed");

  const engine::CoordinateSpace space(1.5, 2, 50.0, 40.0);
  require(std::abs(space.pixels_per_twip() - 0.1) < 0.00001,
          "150% zoom must map ten twips to one logical pixel");
  const auto document_x = space.pixel_x_to_twips(173.4);
  require(std::abs(space.twips_x_to_pixels(document_x) - 173.4) <= 0.051,
          "coordinate round trip exceeded half a twip");
  const auto rectangle = space.pixel_rect_to_twips(50.0, 40.0, 100.0, 200.0);
  require(rectangle == engine::Rect{0, 0, 1000, 2000},
          "pixel rectangle conversion is not stable");

  canvas::TileCache cache(100);
  const engine::TileRequest first_request{.document_area_twips = {0, 0, 100, 100},
                                          .pixel_width = 5,
                                          .pixel_height = 5,
                                          .zoom = 1.0,
                                          .device_scale = 1,
                                          .revision = 1};
  auto second_request = first_request;
  second_request.document_area_twips.x = 100;
  auto third_request = first_request;
  third_request.document_area_twips.x = 200;

  cache.insert(canvas::tile_key(first_request), make_tile(40));
  cache.insert(canvas::tile_key(second_request), make_tile(40));
  require(cache.find(canvas::tile_key(first_request)) != nullptr, "cache lookup missed");
  cache.insert(canvas::tile_key(third_request), make_tile(40));
  require(cache.find(canvas::tile_key(second_request)) == nullptr,
          "least-recently-used tile was not evicted");
  require(cache.find(canvas::tile_key(first_request)) != nullptr,
          "recent tile was evicted instead of the least recent tile");

  cache.invalidate(engine::Rect{0, 0, 50, 50}, 1);
  require(cache.find(canvas::tile_key(first_request)) == nullptr,
          "overlapping invalidation left a stale tile");
  require(cache.find(canvas::tile_key(third_request)) != nullptr,
          "non-overlapping invalidation discarded an unrelated tile");

  auto revised_request = third_request;
  revised_request.revision = 2;
  cache.invalidate(std::nullopt, revised_request.revision);
  require(cache.size() == 0, "full invalidation did not empty the cache");

  std::cout << "engine_core_test: all checks passed\n";
  return EXIT_SUCCESS;
}
