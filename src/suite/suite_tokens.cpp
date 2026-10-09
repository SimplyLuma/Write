#include "suite/suite_tokens.h"

#include <algorithm>
#include <iterator>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace luma::suite {
namespace {

constexpr RgbaColor rgb(const std::uint8_t red, const std::uint8_t green,
                        const std::uint8_t blue) {
  return RgbaColor{red, green, blue, 1.0};
}

constexpr RgbaColor rgba(const std::uint8_t red, const std::uint8_t green,
                         const std::uint8_t blue, const double alpha) {
  return RgbaColor{red, green, blue, alpha};
}

// Fable's shared geometry. Both treatments use it unchanged: a theme changes
// colour, never proportion.
constexpr SuiteGeometry kSharedGeometry{
    .reference_width = 1440,
    .reference_height = 900,
    .compact_width = 1280,
    .compact_height = 720,
    .studio_bar_height = {46, 50, 52},
    .command_bar_height = {42, 44, 46},
    .status_bar_height = {26, 28, 30},
    // Measured from the design's own rendering at the 1440 reference width,
    // where the outline sits at 180 and the properties pane at 262. The design
    // is the authority on proportion, so these replace the wider ranges this
    // file used to carry.
    .navigator_width = {168, 180, 208},
    .inspector_width = {240, 262, 300},
    .minimum_canvas_width = 640,
    .spacing = {},
    .panel_radius = 15.0,
    .control_radius = 10.0,
    .page_radius = 2.5,
    .compact_maximum_open_side_panels = 1,
};

// Light. Values marked canonical are lifted from the Fable stylesheet's
// .f-appbody scope; the rest are derived against it.
const SuiteTokens kLightTokens{
    .colors =
        {
            .main_chrome = rgb(0xFB, 0xFB, 0xFA),      // canonical --f-surface
            .secondary_chrome = rgb(0xF1, 0xF2, 0xF3),  // --f-fill over surface
            .canvas = rgb(0xE4, 0xE6, 0xE8),
            .page = rgb(0xFF, 0xFF, 0xFF),
            .primary_ink = rgb(0x28, 0x30, 0x38),      // canonical --f-ink
            .secondary_ink = rgb(0x4A, 0x54, 0x5E),
            .quiet_ink = rgb(0x75, 0x7E, 0x87),        // canonical --f-muted
            .hairline = rgba(0x27, 0x30, 0x38, 0.095),  // canonical --f-line
            .strong_hairline = rgba(0x27, 0x30, 0x38, 0.18),
            .accent = rgb(0x52, 0x7F, 0xAE),           // canonical --f-blue
            .accent_hover = rgb(0x46, 0x6F, 0x99),     // .f-button.primary:hover
            .selection_tint = rgba(0x52, 0x7F, 0xAE, 0.16),
            .warning = rgb(0xB1, 0x7A, 0x3C),          // canonical --f-amber
            .destructive = rgb(0xA6, 0x49, 0x4D),      // .f-button.danger
            .success = rgb(0x3B, 0x89, 0x67),          // canonical --f-green
        },
    .geometry = kSharedGeometry,
    .typography = {},
};

// Dark. The page stays white: a word processor shows paper, and inverting it
// would leave the writer proofing something the printer will never produce.
// Only the chrome darkens.
const SuiteTokens kDarkTokens{
    .colors =
        {
            .main_chrome = rgb(0x1B, 0x1F, 0x23),
            .secondary_chrome = rgb(0x23, 0x28, 0x2D),
            .canvas = rgb(0x13, 0x16, 0x19),
            .page = rgb(0xFF, 0xFF, 0xFF),
            .primary_ink = rgb(0xE4, 0xE8, 0xEA),
            .secondary_ink = rgb(0xB8, 0xC0, 0xC6),
            .quiet_ink = rgb(0x8B, 0x94, 0x9C),
            .hairline = rgba(0xFF, 0xFF, 0xFF, 0.09),
            .strong_hairline = rgba(0xFF, 0xFF, 0xFF, 0.16),
            .accent = rgb(0x7E, 0xA8, 0xD2),
            .accent_hover = rgb(0x95, 0xB8, 0xDC),
            .selection_tint = rgba(0x7E, 0xA8, 0xD2, 0.18),
            .warning = rgb(0xD1, 0x94, 0x47),
            .destructive = rgb(0xC2, 0x6B, 0x6F),
            .success = rgb(0x65, 0xB5, 0x8C),
        },
    .geometry = kSharedGeometry,
    .typography = {},
};

bool valid_range(const ExtentRange& range) {
  return range.minimum > 0 && range.minimum <= range.preferred &&
         range.preferred <= range.maximum;
}

void validate_color(const RgbaColor& color, const char* name,
                    std::vector<std::string>& problems) {
  if (!std::isfinite(color.alpha) || color.alpha < 0.0 || color.alpha > 1.0) {
    problems.emplace_back(std::string{name} + " alpha must be between 0 and 1");
  }
}

}  // namespace

const SuiteTokens& suite_tokens(const SuiteTheme theme) {
  return theme == SuiteTheme::kDark ? kDarkTokens : kLightTokens;
}

const SuiteTokens& default_suite_tokens() {
  return suite_tokens(SuiteTheme::kLight);
}

std::vector<std::string> validate_tokens(const SuiteTokens& tokens) {
  std::vector<std::string> problems;
  const auto& geometry = tokens.geometry;
  if (geometry.reference_width <= 0 || geometry.reference_height <= 0 ||
      geometry.compact_width <= 0 || geometry.compact_height <= 0) {
    problems.emplace_back("reference and compact sizes must be positive");
  }
  if (geometry.compact_width > geometry.reference_width ||
      geometry.compact_height > geometry.reference_height) {
    problems.emplace_back("compact size must not exceed the reference size");
  }
  const std::pair<const char*, const ExtentRange*> ranges[] = {
      {"studio_bar_height", &geometry.studio_bar_height},
      {"command_bar_height", &geometry.command_bar_height},
      {"status_bar_height", &geometry.status_bar_height},
      {"navigator_width", &geometry.navigator_width},
      {"inspector_width", &geometry.inspector_width},
  };
  for (const auto& [name, range] : ranges) {
    if (!valid_range(*range)) {
      problems.emplace_back(std::string{name} +
                            " must satisfy 0 < min <= preferred <= max");
    }
  }
  if (geometry.minimum_canvas_width <= 0) {
    problems.emplace_back("minimum_canvas_width must be positive");
  }
  if (geometry.panel_radius < 0.0 || geometry.control_radius < 0.0 ||
      geometry.page_radius < 0.0) {
    problems.emplace_back("corner radii must not be negative");
  }
  if (geometry.compact_maximum_open_side_panels != 1) {
    problems.emplace_back(
        "compact layouts must allow exactly one open side panel");
  }

  // The spacing ramp only means anything if it stays a ramp. A step that sits
  // out of order lets ad-hoc padding back in wearing a token's name.
  const auto& spacing = geometry.spacing;
  const int steps[] = {spacing.hairline_gap, spacing.tight, spacing.snug,
                       spacing.base,         spacing.roomy, spacing.loose};
  if (steps[0] <= 0) {
    problems.emplace_back("spacing steps must be positive");
  }
  for (std::size_t index = 1; index < std::size(steps); ++index) {
    if (steps[index] <= steps[index - 1]) {
      problems.emplace_back("spacing steps must increase strictly");
      break;
    }
  }

  const auto& type = tokens.typography;
  const int sizes[] = {type.label, type.body, type.heading, type.title,
                       type.display};
  if (sizes[0] <= 0) {
    problems.emplace_back("type sizes must be positive");
  }
  for (std::size_t index = 1; index < std::size(sizes); ++index) {
    if (sizes[index] <= sizes[index - 1]) {
      problems.emplace_back("type sizes must increase strictly");
      break;
    }
  }
  if (type.family == nullptr || *type.family == '\0') {
    problems.emplace_back("typography must name a family");
  }

  const std::pair<const char*, const RgbaColor*> colors[] = {
      {"main_chrome", &tokens.colors.main_chrome},
      {"secondary_chrome", &tokens.colors.secondary_chrome},
      {"canvas", &tokens.colors.canvas},
      {"page", &tokens.colors.page},
      {"primary_ink", &tokens.colors.primary_ink},
      {"secondary_ink", &tokens.colors.secondary_ink},
      {"quiet_ink", &tokens.colors.quiet_ink},
      {"hairline", &tokens.colors.hairline},
      {"strong_hairline", &tokens.colors.strong_hairline},
      {"accent", &tokens.colors.accent},
      {"accent_hover", &tokens.colors.accent_hover},
      {"selection_tint", &tokens.colors.selection_tint},
      {"warning", &tokens.colors.warning},
      {"destructive", &tokens.colors.destructive},
      {"success", &tokens.colors.success},
  };
  for (const auto& [name, color] : colors) {
    validate_color(*color, name, problems);
  }
  return problems;
}

std::string to_css_color(const RgbaColor& color) {
  std::ostringstream output;
  if (color.alpha >= 1.0) {
    output << '#' << std::uppercase << std::hex << std::setfill('0')
           << std::setw(2) << static_cast<int>(color.red) << std::setw(2)
           << static_cast<int>(color.green) << std::setw(2)
           << static_cast<int>(color.blue);
    return output.str();
  }
  output << "rgba(" << static_cast<int>(color.red) << ','
         << static_cast<int>(color.green) << ','
         << static_cast<int>(color.blue) << ',' << std::fixed
         << std::setprecision(2) << std::clamp(color.alpha, 0.0, 1.0) << ')';
  return output.str();
}

}  // namespace luma::suite
