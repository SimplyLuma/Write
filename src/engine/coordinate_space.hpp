#pragma once

#include "engine_types.hpp"

#include <cstdint>

namespace luma::write::engine {

// LibreOfficeKit uses absolute twips (1/1440 inch). GTK uses logical pixels.
// This class is the only coordinate conversion boundary used by the UI.
class CoordinateSpace {
 public:
  static constexpr double kTwipsPerInch = 1440.0;
  static constexpr double kCssPixelsPerInch = 96.0;

  CoordinateSpace(double zoom, int device_scale, double origin_x_pixels = 0.0,
                  double origin_y_pixels = 0.0);

  [[nodiscard]] double zoom() const noexcept { return zoom_; }
  [[nodiscard]] int device_scale() const noexcept { return device_scale_; }
  [[nodiscard]] double pixels_per_twip() const noexcept;
  [[nodiscard]] double twips_per_pixel() const noexcept;

  [[nodiscard]] std::int64_t pixel_x_to_twips(double x) const noexcept;
  [[nodiscard]] std::int64_t pixel_y_to_twips(double y) const noexcept;
  [[nodiscard]] double twips_x_to_pixels(std::int64_t x) const noexcept;
  [[nodiscard]] double twips_y_to_pixels(std::int64_t y) const noexcept;
  [[nodiscard]] Rect pixel_rect_to_twips(double x, double y, double width,
                                         double height) const noexcept;

 private:
  double zoom_{1.0};
  int device_scale_{1};
  double origin_x_pixels_{0.0};
  double origin_y_pixels_{0.0};
};

}  // namespace luma::write::engine
