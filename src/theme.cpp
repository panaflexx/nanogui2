/*
    src/theme.cpp -- Storage class for theme-related properties

    The text box widget was contributed by Christian Schueller.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <nanogui/icons.h>
#include <nanogui_resources.h>
#include <stdexcept>
#include <algorithm>

NAMESPACE_BEGIN(nanogui)

Theme::Theme(NVGcontext *ctx, ThemeMode mode) {
    // Safe defaults before fonts load
    m_font_sans_regular = m_font_sans_bold = m_font_icons =
        m_font_mono_regular = m_font_emoji = -1;
    m_icon_scale = 0.60f;

    apply_common_metrics();

    m_check_box_icon               = FA_CHECK;
    m_message_information_icon     = FA_INFO_CIRCLE;
    m_message_question_icon        = FA_QUESTION_CIRCLE;
    m_message_warning_icon         = FA_EXCLAMATION_TRIANGLE;
    m_message_alt_button_icon      = FA_TIMES_CIRCLE;
    m_message_primary_button_icon  = FA_CHECK;
    m_popup_chevron_right_icon     = FA_CHEVRON_RIGHT;
    m_popup_chevron_left_icon      = FA_CHEVRON_LEFT;
    m_text_box_up_icon             = FA_CHEVRON_UP;
    m_text_box_down_icon           = FA_CHEVRON_DOWN;

    load_fonts(ctx);
    set_mode(mode);
}

void Theme::set_mode(ThemeMode mode) {
    m_mode = mode;
    apply_common_metrics();
    if (mode == ThemeMode::Light)
        apply_light_palette();
    else
        apply_dark_palette();
}

void Theme::apply_common_metrics() {
    // macOS Glass proportions: larger continuous radii, roomy title bars,
    // soft deep shadows, compact but touchable controls.
    m_standard_font_size            = 15;
    m_button_font_size              = 15;
    m_text_box_font_size            = 15;
    m_heading_font_size             = 20;
    m_title_font_size               = 24;
    m_caption_font_size             = 13;
    m_muted_font_size               = 12;

    m_control_height                = 28;
    m_control_min_width             = 120;
    m_control_padding_x             = 10;
    m_form_label_width              = 140;
    m_row_spacing                   = 8;
    m_section_spacing               = 16;

    m_window_corner_radius          = 14;
    m_window_header_height          = 36;
    m_window_drop_shadow_size       = 28;
    m_button_corner_radius          = 8;
    m_tab_border_width              = 0.5f;
    m_tab_inner_margin              = 5;
    m_tab_min_button_width          = 20;
    m_tab_max_button_width          = 160;
    m_tab_control_width             = 20;
    m_tab_button_horizontal_padding = 12;
    m_tab_button_vertical_padding   = 6;
    m_resize_area_offset            = 10;
    m_split_divider_width           = 1;
    m_menu_item_corner_radius       = 6;
    m_icon_scale                    = 0.60f;
}

void Theme::apply_dark_palette() {
    // macOS dark glass — deep charcoal with cool translucency and #0A84FF accent.
    m_drop_shadow                   = Color(0, 0, 0, 110);
    m_transparent                   = Color(0, 0);
    m_border_dark                   = Color(0, 0, 0, 90);
    m_border_light                  = Color(255, 255, 255, 40);
    m_border_medium                 = Color(255, 255, 255, 22);

    m_text_color                    = Color(245, 245, 247, 255);
    m_disabled_text_color           = Color(255, 255, 255, 90);
    m_text_color_shadow             = Color(0, 0); // no hard text shadow on glass
    m_text_secondary                = Color(174, 174, 178, 255);
    m_text_heading                  = Color(255, 255, 255, 245);
    m_text_muted                    = Color(142, 142, 147, 255);
    m_text_accent                   = Color(10, 132, 255, 255);
    m_icon_color                    = m_text_color;
    m_disabled_icon_color           = Color(255, 255, 255, 70);

    m_success_color                 = Color(48, 209, 88, 255);
    m_danger_color                  = Color(255, 69, 58, 255);
    m_fail_color                    = m_danger_color;
    m_proceed_color                 = Color(10, 132, 255, 255);
    m_warning_color                 = Color(255, 214, 10, 255);
    m_accent_color                  = Color(10, 132, 255, 255);
    m_button_text_on_solid          = Color(255, 255);

    m_glass_specular                = Color(255, 255, 255, 28);
    m_glass_border                  = Color(255, 255, 255, 36);
    m_focus_ring                    = Color(10, 132, 255, 160);
    m_scrollbar_thumb               = Color(255, 255, 255, 70);
    m_scrollbar_thumb_active        = Color(255, 255, 255, 140);
    m_track_color                   = Color(255, 255, 255, 28);
    m_track_fill_color              = m_accent_color;
    m_checkbox_bg                   = Color(255, 255, 255, 22);
    m_checkbox_border               = Color(255, 255, 255, 55);

    // Cool deep desktop wallpaper tone
    m_screen_background             = Color(28, 28, 32, 255);

    // Buttons: frosted translucent slabs (near-flat top/bot = soft glass, not bevel)
    m_button_gradient_top_focused   = Color(255, 255, 255, 48);
    m_button_gradient_bot_focused   = Color(255, 255, 255, 32);
    m_button_gradient_top_unfocused = Color(255, 255, 255, 28);
    m_button_gradient_bot_unfocused = Color(255, 255, 255, 18);
    m_button_gradient_top_pushed    = Color(255, 255, 255, 16);
    m_button_gradient_bot_pushed    = Color(0, 0, 0, 40);

    // Windows: translucent charcoal glass
    m_window_fill_unfocused         = Color(44, 44, 48, 230);
    m_window_fill_focused           = Color(50, 50, 56, 236);
    m_window_title_unfocused        = Color(174, 174, 178, 255);
    m_window_title_focused          = Color(245, 245, 247, 255);
    m_window_header_gradient_top    = Color(60, 60, 68, 240);
    m_window_header_gradient_bot    = Color(48, 48, 54, 220);
    m_window_header_sep_top         = Color(255, 255, 255, 30);
    m_window_header_sep_bot         = Color(0, 0, 0, 50);
    m_window_popup                  = Color(48, 48, 54, 245);
    m_window_popup_transparent      = Color(48, 48, 54, 0);

    m_split_divider                 = Color(255, 255, 255, 28);

    m_menubar_fill                  = Color(36, 36, 40, 240);
    m_menubar_button_focused        = Color(10, 132, 255, 220);
    m_menu_popup_fill               = Color(42, 42, 48, 250);
    m_menu_separator                = Color(255, 255, 255, 28);

    m_text_box_bg_top               = Color(0, 0, 0, 70);
    m_text_box_bg_bot               = Color(0, 0, 0, 55);
    m_text_box_focused_bg_top       = Color(0, 0, 0, 90);
    m_text_box_focused_bg_bot       = Color(10, 40, 70, 70);
}

void Theme::apply_light_palette() {
    // macOS light glass — airy translucent white over cool gray desktop, #007AFF.
    m_drop_shadow                   = Color(0, 0, 0, 55);
    m_transparent                   = Color(0, 0);
    m_border_dark                   = Color(0, 0, 0, 28);
    m_border_light                  = Color(255, 255, 255, 200);
    m_border_medium                 = Color(0, 0, 0, 18);

    m_text_color                    = Color(29, 29, 31, 255);
    m_disabled_text_color           = Color(60, 60, 67, 90);
    m_text_color_shadow             = Color(0, 0);
    m_text_secondary                = Color(60, 60, 67, 180);
    m_text_heading                  = Color(0, 0, 0, 230);
    m_text_muted                    = Color(60, 60, 67, 140);
    m_text_accent                   = Color(0, 122, 255, 255);
    m_icon_color                    = Color(0, 122, 255, 255);
    m_disabled_icon_color           = Color(60, 60, 67, 80);

    m_success_color                 = Color(52, 199, 89, 255);
    m_danger_color                  = Color(255, 59, 48, 255);
    m_fail_color                    = m_danger_color;
    m_proceed_color                 = Color(0, 122, 255, 255);
    m_warning_color                 = Color(255, 204, 0, 255);
    m_accent_color                  = Color(0, 122, 255, 255);
    m_button_text_on_solid          = Color(255, 255);

    m_glass_specular                = Color(255, 255, 255, 140);
    m_glass_border                  = Color(255, 255, 255, 180);
    m_focus_ring                    = Color(0, 122, 255, 150);
    m_scrollbar_thumb               = Color(0, 0, 0, 50);
    m_scrollbar_thumb_active        = Color(0, 0, 0, 110);
    m_track_color                   = Color(0, 0, 0, 28);
    m_track_fill_color              = m_accent_color;
    m_checkbox_bg                   = Color(255, 255, 255, 180);
    m_checkbox_border               = Color(0, 0, 0, 40);

    // Soft cool desktop
    m_screen_background             = Color(220, 224, 232, 255);

    // Buttons: bright frosted glass
    m_button_gradient_top_focused   = Color(255, 255, 255, 230);
    m_button_gradient_bot_focused   = Color(240, 244, 250, 220);
    m_button_gradient_top_unfocused = Color(255, 255, 255, 200);
    m_button_gradient_bot_unfocused = Color(245, 247, 250, 185);
    m_button_gradient_top_pushed    = Color(220, 228, 240, 220);
    m_button_gradient_bot_pushed    = Color(200, 210, 225, 210);

    // Windows: milky glass panels
    m_window_fill_unfocused         = Color(250, 251, 253, 220);
    m_window_fill_focused           = Color(255, 255, 255, 235);
    m_window_title_unfocused        = Color(60, 60, 67, 160);
    m_window_title_focused          = Color(29, 29, 31, 255);
    m_window_header_gradient_top    = Color(255, 255, 255, 245);
    m_window_header_gradient_bot    = Color(242, 244, 248, 220);
    m_window_header_sep_top         = Color(255, 255, 255, 200);
    m_window_header_sep_bot         = Color(0, 0, 0, 22);
    m_window_popup                  = Color(255, 255, 255, 245);
    m_window_popup_transparent      = Color(255, 0);

    m_split_divider                 = Color(0, 0, 0, 28);

    m_menubar_fill                  = Color(250, 251, 253, 230);
    m_menubar_button_focused        = Color(0, 122, 255, 255);
    m_menu_popup_fill               = Color(255, 255, 255, 250);
    m_menu_separator                = Color(0, 0, 0, 28);

    m_text_box_bg_top               = Color(255, 255, 255, 200);
    m_text_box_bg_bot               = Color(245, 247, 250, 180);
    m_text_box_focused_bg_top       = Color(255, 255, 255, 245);
    m_text_box_focused_bg_bot       = Color(240, 246, 255, 230);
}

void Theme::configure_as_menubar() {
    // Glass strip: no window chrome, transparent ghost buttons until hover
    m_window_corner_radius          = 0;
    m_window_header_height          = 0;
    m_window_drop_shadow_size       = 10;
    m_button_corner_radius          = 6;
    m_drop_shadow                   = Color(0, 0, 0, 60);

    m_window_fill_unfocused         = m_menubar_fill;
    m_window_fill_focused           = m_menubar_fill;
    m_window_popup                  = m_menu_popup_fill;

    m_border_light                  = m_transparent;
    m_border_dark                   = m_transparent;
    m_border_medium                 = m_transparent;
    m_button_gradient_top_unfocused = m_transparent;
    m_button_gradient_bot_unfocused = m_transparent;
    m_button_gradient_top_focused   = m_menubar_button_focused;
    m_button_gradient_bot_focused   = m_menubar_button_focused;
    m_button_gradient_top_pushed    = m_menubar_button_focused;
    m_button_gradient_bot_pushed    = m_menubar_button_focused;
    m_text_color_shadow             = m_transparent;
}

void Theme::draw_glass_shadow(NVGcontext *ctx, float x, float y, float w, float h,
                              float radius, float shadow_size) const {
    float ds = shadow_size >= 0.f ? shadow_size : (float)m_window_drop_shadow_size;
    if (ds < 1.f)
        return;

    // Soft ambient shadow slightly below the surface (macOS contact shadow)
    NVGpaint ambient = nvgBoxGradient(
        ctx, x, y + ds * 0.15f, w, h, radius * 1.2f, ds * 1.6f,
        m_drop_shadow, m_transparent);

    nvgBeginPath(ctx);
    nvgRect(ctx, x - ds, y - ds * 0.4f, w + 2.f * ds, h + 2.2f * ds);
    nvgRoundedRect(ctx, x, y, w, h, radius);
    nvgPathWinding(ctx, NVG_HOLE);
    nvgFillPaint(ctx, ambient);
    nvgFill(ctx);
}

void Theme::draw_glass_surface(NVGcontext *ctx, float x, float y, float w, float h,
                               float radius, const Color &fill,
                               bool specular, bool border) const {
    // Base fill
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, x, y, w, h, radius);
    nvgFillColor(ctx, fill);
    nvgFill(ctx);

    if (specular && h > 2.f) {
        // Soft top-edge specular — simulates light catching the glass rim
        float band = std::min(h * 0.45f, 22.f);
        NVGpaint wash = nvgLinearGradient(
            ctx, x, y, x, y + band,
            m_glass_specular, m_transparent);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x + 0.5f, y + 0.5f, w - 1.f, band, radius);
        nvgFillPaint(ctx, wash);
        nvgFill(ctx);
    }

    if (border) {
        // Hairline glass edge
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x + 0.5f, y + 0.5f, w - 1.f, h - 1.f, std::max(0.f, radius - 0.5f));
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, m_glass_border);
        nvgStroke(ctx);

        // Subtle inner dark edge for definition on light backgrounds
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x + 1.f, y + 1.f, w - 2.f, h - 2.f, std::max(0.f, radius - 1.f));
        nvgStrokeWidth(ctx, 0.5f);
        nvgStrokeColor(ctx, m_border_dark);
        nvgStroke(ctx);
    }
}

void Theme::draw_focus_ring(NVGcontext *ctx, float x, float y, float w, float h,
                            float radius) const {
    // Outer soft glow
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, x - 1.5f, y - 1.5f, w + 3.f, h + 3.f, radius + 1.5f);
    nvgStrokeWidth(ctx, 3.f);
    Color soft = m_focus_ring;
    soft.a() *= 0.35f;
    nvgStrokeColor(ctx, soft);
    nvgStroke(ctx);

    // Crisp ring
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, x - 0.5f, y - 0.5f, w + 1.f, h + 1.f, radius + 0.5f);
    nvgStrokeWidth(ctx, 2.f);
    nvgStrokeColor(ctx, m_focus_ring);
    nvgStroke(ctx);
}

void Theme::load_fonts(NVGcontext *ctx) {
    m_font_sans_regular = nvgCreateFontMem(ctx, "sans", (uint8_t *) roboto_regular_ttf,
                                           roboto_regular_ttf_size, 0);
    m_font_sans_bold = nvgCreateFontMem(ctx, "sans-bold", (uint8_t *) roboto_bold_ttf,
                                        roboto_bold_ttf_size, 0);
    m_font_icons = nvgCreateFontMem(ctx, "icons", (uint8_t *) fontawesome_solid_ttf,
                                    fontawesome_solid_ttf_size, 0);
    m_font_mono_regular = nvgCreateFontMem(ctx, "mono", (uint8_t *) inconsolata_regular_ttf,
                                           inconsolata_regular_ttf_size, 0);
    m_font_emoji = nvgCreateFont(ctx, "emoji", "resources/NotoColorEmoji.ttf");
    nvgSetEmojiFont(ctx, "emoji");

    if (m_font_sans_regular == -1 || m_font_sans_bold == -1 ||
        m_font_icons == -1 || m_font_mono_regular == -1 ||
        m_font_emoji == -1)
        throw std::runtime_error("Could not load fonts!");
}

NAMESPACE_END(nanogui)
