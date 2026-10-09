#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::suite {

/** Toolkit-neutral color value shared by every Luma suite application. */
struct RgbaColor {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
  double alpha{1.0};

  bool operator==(const RgbaColor&) const = default;
};

/** Which of the two shipped treatments a token bundle describes. */
enum class SuiteTheme {
  kLight,
  kDark,
};

/** Fable palette for one treatment, shared by every Luma suite application. */
struct SuiteColors {
  RgbaColor main_chrome;
  RgbaColor secondary_chrome;
  RgbaColor canvas;
  RgbaColor page;
  RgbaColor primary_ink;
  RgbaColor secondary_ink;
  RgbaColor quiet_ink;
  RgbaColor hairline;
  RgbaColor strong_hairline;
  RgbaColor accent;
  RgbaColor accent_hover;
  RgbaColor selection_tint;
  RgbaColor warning;
  RgbaColor destructive;
  RgbaColor success;

  bool operator==(const SuiteColors&) const = default;
};

/**
 * Fable's spacing ramp. Every gap and inset in the suite is one of these six
 * steps; a measurement that is not on the ramp is a mistake, not a refinement.
 */
struct SuiteSpacing {
  int hairline_gap{4};
  int tight{6};
  int snug{8};
  int base{12};
  int roomy{16};
  int loose{24};

  bool operator==(const SuiteSpacing&) const = default;
};

/**
 * Fable's type ramp, in Figtree. The suite ships the family rather than hoping
 * the host has it, so `family` names the face and `fallback` covers the gap
 * before fontconfig picks it up.
 */
struct SuiteTypography {
  const char* family{"Figtree"};
  const char* fallback{"Inter, Cantarell, system-ui, sans-serif"};
  int label{11};
  int body{13};
  int heading{14};
  int title{19};
  int display{29};

  bool operator==(const SuiteTypography&) const = default;
};

/** Inclusive extent contract for a shell surface, measured in CSS pixels. */
struct ExtentRange {
  int minimum{0};
  int preferred{0};
  int maximum{0};

  bool operator==(const ExtentRange&) const = default;
};

/** Shared shell geometry and responsive behavior. */
struct SuiteGeometry {
  int reference_width{1440};
  int reference_height{900};
  int compact_width{1280};
  int compact_height{720};
  ExtentRange studio_bar_height;
  ExtentRange command_bar_height;
  ExtentRange status_bar_height;
  ExtentRange navigator_width;
  ExtentRange inspector_width;
  int minimum_canvas_width{640};
  SuiteSpacing spacing;
  double panel_radius{15.0};
  double control_radius{10.0};
  double page_radius{2.5};
  int compact_maximum_open_side_panels{1};

  bool operator==(const SuiteGeometry&) const = default;
};

/** Complete toolkit-neutral token bundle used to derive GTK named colors/CSS. */
struct SuiteTokens {
  SuiteColors colors;
  SuiteGeometry geometry;
  SuiteTypography typography;

  bool operator==(const SuiteTokens&) const = default;
};

/** Returns the token bundle for one treatment. */
const SuiteTokens& suite_tokens(SuiteTheme theme);

/** Returns the light bundle. Equivalent to suite_tokens(SuiteTheme::kLight). */
const SuiteTokens& default_suite_tokens();

/** Reports invalid ranges, alphas, radii, or responsive geometry. */
std::vector<std::string> validate_tokens(const SuiteTokens& tokens);

/** Formats a color as #RRGGBB or rgba(r,g,b,a), useful for CSS adapters. */
std::string to_css_color(const RgbaColor& color);

}  // namespace luma::suite
