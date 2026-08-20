/*
    The text box widget was contributed by Christian Schueller.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/**
 * \file nanogui/theme.h
 *
 * \brief Storage class for theme-related properties (colors, metrics, fonts).
 *
 * Default visual language is macOS Glass: translucent surfaces, soft shadows,
 * large continuous corner radii, specular top edges, and system-blue accents.
 */

#pragma once

#include <nanogui/vector.h>
#include <nanogui/object.h>

struct NVGcontext;

NAMESPACE_BEGIN(nanogui)

/// Global light / dark appearance for a Theme palette.
enum class ThemeMode {
    Dark = 0,
    Light
};

/**
 * \class Theme theme.h nanogui/theme.h
 *
 * \brief Storage class for theme-related properties.
 *
 * Construct with a \ref ThemeMode (default: Dark). Call \ref set_mode to
 * switch light/dark palettes at runtime — fonts are kept, colors and
 * mode-dependent metrics are replaced. Use \ref Screen::set_theme_mode to
 * apply a mode to a live UI tree.
 *
 * Both modes use a macOS Glass material language (frosted translucent panels,
 * hairline specular borders, soft layered shadows, system accent).
 */
class NANOGUI_EXPORT Theme : public Object {
public:
    /**
     * \param ctx  NanoVG context used to load embedded fonts
     * \param mode Initial light/dark palette (default Dark for classic look)
     */
    Theme(NVGcontext *ctx, ThemeMode mode = ThemeMode::Dark);

    /// Current light/dark mode.
    ThemeMode mode() const { return m_mode; }

    /**
     * \brief Replace the palette with the light or dark preset.
     *
     * Fonts and icons are preserved. Mode-independent structural defaults
     * (corner radii, control height, etc.) are also refreshed so callers get
     * a coherent preset.
     */
    void set_mode(ThemeMode mode);

    /**
     * \brief Configure this theme for use as a MenuBar skin.
     *
     * Call after \ref set_mode (or construction). Makes window fill match the
     * menu bar strip, buttons transparent until hover, no window header/shadow.
     * Safe to call repeatedly when the parent mode changes.
     */
    void configure_as_menubar();

    /* ---- Glass drawing helpers -------------------------------------------- */
    /**
     * Soft multi-layer drop shadow behind a rounded rectangle (macOS depth).
     * Draws outside the rect; caller should \c nvgResetScissor if needed.
     */
    void draw_glass_shadow(NVGcontext *ctx, float x, float y, float w, float h,
                           float radius, float shadow_size = -1.f) const;

    /**
     * Frosted glass surface: base fill, optional top specular wash, hairline border.
     */
    void draw_glass_surface(NVGcontext *ctx, float x, float y, float w, float h,
                            float radius, const Color &fill,
                            bool specular = true, bool border = true) const;

    /**
     * System-blue focus ring (outer soft glow + crisp inner stroke).
     */
    void draw_focus_ring(NVGcontext *ctx, float x, float y, float w, float h,
                         float radius) const;

    /* ---- Fonts ------------------------------------------------------------ */
    /// The standard font face (default: ``"sans"`` from ``resources/roboto_regular.ttf``).
    int m_font_sans_regular;
    /// The bold font face (default: ``"sans-bold"`` from ``resources/roboto_bold.ttf``).
    int m_font_sans_bold;
    /// Italic face: alias of ``"sans"`` (no italic Roboto variant is vendored;
    /// exists so ``Document::faceForStyle`` never names an unregistered face).
    int m_font_sans_italic;
    /// Bold-italic face: alias of ``"sans-bold"`` (see m_font_sans_italic).
    int m_font_sans_bolditalic;
    /// The icon font face (default: ``"icons"`` from Font Awesome).
    int m_font_icons;
    /// The monospace font face (default: ``"mono"`` from ``resources/inconsolata_regular.ttf``).
    int m_font_mono_regular;
    /// The emoji font face (default: ``"emoji"`` from ``resources/NotoColorEmoji.ttf``).
    int m_font_emoji;
    /**
     * Icon scaling relative to widget font size (default: ``0.60f``).
     */
    float m_icon_scale;

    /* ---- Typography ------------------------------------------------------- */
    /// Body / label font size (default: 16).
    int m_standard_font_size;
    /// Button caption font size (default: 15).
    int m_button_font_size;
    /// TextBox / numeric field font size (default: 15).
    int m_text_box_font_size;
    /// Section heading font size (default: 20).
    int m_heading_font_size;
    /// Large title font size (default: 24).
    int m_title_font_size;
    /// Form caption / field-label font size (default: 13).
    int m_caption_font_size;
    /// Status / muted footer font size (default: 12).
    int m_muted_font_size;

    /* ---- Control metrics -------------------------------------------------- */
    /// Preferred / min height for buttons, text boxes, dropdowns (default: 28).
    int m_control_height;
    /// Preferred min width for empty text boxes / combos (default: 120).
    int m_control_min_width;
    /// Horizontal padding inside text-like controls (default: 10).
    int m_control_padding_x;
    /// Default form label column width when laying out rows (default: 140).
    int m_form_label_width;
    /// Default spacing between form rows (default: 8).
    int m_row_spacing;
    /// Default spacing between form sections (default: 16).
    int m_section_spacing;
    /// Rounding radius for Window widget corners (default: 14).
    int m_window_corner_radius;
    /// Window title bar height (default: 36).
    int m_window_header_height;
    /// Drop shadow size behind Window widgets (default: 28).
    int m_window_drop_shadow_size;
    /// Rounding radius for Button (and derived) widgets (default: 8).
    int m_button_corner_radius;
    /// Tab header border width (default: 0.5f).
    float m_tab_border_width;
    /// Tab header inner margin (default: 5).
    int m_tab_inner_margin;
    /// Min tab button width (default: 20).
    int m_tab_min_button_width;
    /// Max tab button width (default: 160).
    int m_tab_max_button_width;
    /// Tab control width bound (default: 20).
    int m_tab_control_width;
    /// Tab button horizontal padding (default: 12).
    int m_tab_button_horizontal_padding;
    /// Tab button vertical padding (default: 6).
    int m_tab_button_vertical_padding;
    /// Window resize grip / edge hit offset (default: 10).
    int m_resize_area_offset;

    /* ---- Generic colors --------------------------------------------------- */
    Color m_drop_shadow;
    Color m_transparent;
    Color m_border_dark;
    Color m_border_light;
    Color m_border_medium;
    /// Primary body text.
    Color m_text_color;
    /// Disabled body text.
    Color m_disabled_text_color;
    /// Soft text shadow under captions (buttons/windows).
    Color m_text_color_shadow;
    /// Form field captions / secondary labels.
    Color m_text_secondary;
    /// Section headings.
    Color m_text_heading;
    /// Muted status / helper text.
    Color m_text_muted;
    /// Accent / link / emphasis text.
    Color m_text_accent;
    /// Icon default color (typically matches text or accent).
    Color m_icon_color;
    Color m_disabled_icon_color;

    /* ---- Semantic / intention colors -------------------------------------- */
    Color m_success_color;   ///< Positive / submit (green)
    Color m_fail_color;      ///< Alias for danger / destructive
    Color m_danger_color;    ///< Destructive / cancel (red)
    Color m_proceed_color;   ///< Primary progressive action (blue)
    Color m_warning_color;   ///< Caution (amber)
    Color m_accent_color;    ///< Brand accent (buttons, selection)

    /* ---- Glass material extras -------------------------------------------- */
    /// Specular top-edge wash (white with low alpha).
    Color m_glass_specular;
    /// Outer hairline border color for glass surfaces.
    Color m_glass_border;
    /// Soft focus-ring / selection glow color (usually accent @ mid alpha).
    Color m_focus_ring;
    /// Scrollbar thumb idle / active.
    Color m_scrollbar_thumb;
    Color m_scrollbar_thumb_active;
    /// Slider / progress track base fill.
    Color m_track_color;
    /// Slider / progress filled portion (defaults to accent).
    Color m_track_fill_color;
    /// Checkbox unchecked glass fill.
    Color m_checkbox_bg;
    /// Checkbox border when unchecked.
    Color m_checkbox_border;

    /* ---- Screen / chrome -------------------------------------------------- */
    /// Top-level Screen clear / background color.
    Color m_screen_background;

    /* ---- Button colors ---------------------------------------------------- */
    Color m_button_gradient_top_focused;
    Color m_button_gradient_bot_focused;
    Color m_button_gradient_top_unfocused;
    Color m_button_gradient_bot_unfocused;
    Color m_button_gradient_top_pushed;
    Color m_button_gradient_bot_pushed;
    /// Default button caption when drawn on a solid semantic fill (auto-contrast falls back to this).
    Color m_button_text_on_solid;

    /* ---- Window colors ---------------------------------------------------- */
    Color m_window_fill_unfocused;
    Color m_window_fill_focused;
    Color m_window_title_unfocused;
    Color m_window_title_focused;
    Color m_window_header_gradient_top;
    Color m_window_header_gradient_bot;
    Color m_window_header_sep_top;
    Color m_window_header_sep_bot;
    Color m_window_popup;
    Color m_window_popup_transparent;

    /* ---- Split ------------------------------------------------------------ */
    Color m_split_divider;
    int m_split_divider_width;

    /* ---- MenuBar / menu strip --------------------------------------------- */
    /// Menu bar strip fill (also used by configure_as_menubar()).
    Color m_menubar_fill;
    /// Hover / open highlight on menu titles.
    Color m_menubar_button_focused;
    /// Popup menu background.
    Color m_menu_popup_fill;
    /// Menu separator color.
    Color m_menu_separator;
    /// Rounded menu-item highlight radius (default: 6).
    int m_menu_item_corner_radius;

    /* ---- Text field chrome ------------------------------------------------ */
    /// TextBox unfocused background gradient top (alpha included).
    Color m_text_box_bg_top;
    Color m_text_box_bg_bot;
    /// TextBox focused background.
    Color m_text_box_focused_bg_top;
    Color m_text_box_focused_bg_bot;

    /* ---- Icons ------------------------------------------------------------ */
    int m_check_box_icon;
    int m_message_information_icon;
    int m_message_question_icon;
    int m_message_warning_icon;
    int m_message_alt_button_icon;
    int m_message_primary_button_icon;
    int m_popup_chevron_right_icon;
    int m_popup_chevron_left_icon;
    int m_text_box_up_icon;
    int m_text_box_down_icon;

protected:
    ThemeMode m_mode = ThemeMode::Dark;

    /// Apply structural metrics shared by light and dark presets.
    void apply_common_metrics();
    /// Apply the dark color palette (and any dark-specific metrics).
    void apply_dark_palette();
    /// Apply the light color palette (and any light-specific metrics).
    void apply_light_palette();
    /// Load embedded fonts (once per Theme instance).
    void load_fonts(NVGcontext *ctx);

    virtual ~Theme() { };
};

NAMESPACE_END(nanogui)
