#include "coordinate_space.hpp"

#include <algorithm>
#include <cmath>

namespace luma::write::engine {

CoordinateSpace::CoordinateSpace(const double zoom, const int device_scale,
                                 const double origin_x_pixels, const double origin_y_pixels)
    : zoom_(std::clamp(zoom, 0.1, 8.0)),
      device_scale_(std::clamp(device_scale, 1, 4)),
      origin_x_pixels_(origin_x_pixels),
      origin_y_pixels_(origin_y_pixels) {}

double CoordinateSpace::pixels_per_twip() const noexcept {
  return (kCssPixelsPerInch * zoom_) / kTwipsPerInch;
}

double CoordinateSpace::twips_per_pixel() const noexcept { return 1.0 / pixels_per_twip(); }

std::int64_t CoordinateSpace::pixel_x_to_twips(const double x) const noexcept {
  return static_cast<std::int64_t>(std::llround((x - origin_x_pixels_) * twips_per_pixel()));
}

std::int64_t CoordinateSpace::pixel_y_to_twips(const double y) const noexcept {
  return static_cast<std::int64_t>(std::llround((y - origin_y_pixels_) * twips_per_pixel()));
}

double CoordinateSpace::twips_x_to_pixels(const std::int64_t x) const noexcept {
  return origin_x_pixels_ + static_cast<double>(x) * pixels_per_twip();
}

double CoordinateSpace::twips_y_to_pixels(const std::int64_t y) const noexcept {
  return origin_y_pixels_ + static_cast<double>(y) * pixels_per_twip();
}

Rect CoordinateSpace::pixel_rect_to_twips(const double x, const double y, const double width,
                                          const double height) const noexcept {
  const auto start_x = pixel_x_to_twips(x);
  const auto start_y = pixel_y_to_twips(y);
  const auto end_x = pixel_x_to_twips(x + std::max(0.0, width));
  const auto end_y = pixel_y_to_twips(y + std::max(0.0, height));
  return {start_x, start_y, std::max<std::int64_t>(0, end_x - start_x),
          std::max<std::int64_t>(0, end_y - start_y)};
}

}  // namespace luma::write::engine
