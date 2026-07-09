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
    m_standard_font_size            = 16;
    m_button_font_size              = 16;
    m_text_box_font_size            = 16;
    m_heading_font_size             = 20;
    m_title_font_size               = 24;
    m_caption_font_size             = 13;
    m_muted_font_size               = 12;

    m_control_height                = 30;
    m_control_min_width             = 120;
    m_control_padding_x             = 10;
    m_form_label_width              = 140;
    m_row_spacing                   = 8;
    m_section_spacing               = 16;

    m_window_corner_radius          = 4;
    m_window_header_height          = 30;
    m_window_drop_shadow_size       = 10;
    m_button_corner_radius          = 4;
    m_tab_border_width              = 0.75f;
    m_tab_inner_margin              = 5;
    m_tab_min_button_width          = 20;
    m_tab_max_button_width          = 160;
    m_tab_control_width             = 20;
    m_tab_button_horizontal_padding = 10;
    m_tab_button_vertical_padding   = 2;
    m_resize_area_offset            = 10;
    m_split_divider_width           = 4;
    m_icon_scale                    = 0.60f;
}

void Theme::apply_dark_palette() {
    m_drop_shadow                   = Color(0, 128);
    m_transparent                   = Color(0, 0);
    m_border_dark                   = Color(29, 255);
    m_border_light                  = Color(92, 255);
    m_border_medium                 = Color(35, 255);

    m_text_color                    = Color(255, 220);
    m_disabled_text_color           = Color(255, 100);
    m_text_color_shadow             = Color(0, 160);
    m_text_secondary                = Color(200, 180);
    m_text_heading                  = Color(230, 240);
    m_text_muted                    = Color(170, 140);
    m_text_accent                   = Color(100, 180, 255, 255);
    m_icon_color                    = m_text_color;
    m_disabled_icon_color           = Color(120, 100);

    m_success_color                 = Color(40, 167, 69, 255);
    m_danger_color                  = Color(220, 53, 69, 255);
    m_fail_color                    = m_danger_color;
    m_proceed_color                 = Color(52, 144, 220, 255);
    m_warning_color                 = Color(255, 193, 7, 255);
    m_accent_color                  = Color(64, 164, 232, 255);
    m_button_text_on_solid          = Color(255, 255);

    m_screen_background             = Color(48, 48, 52, 255);

    m_button_gradient_top_focused   = Color(64, 255);
    m_button_gradient_bot_focused   = Color(48, 255);
    m_button_gradient_top_unfocused = Color(74, 255);
    m_button_gradient_bot_unfocused = Color(58, 255);
    m_button_gradient_top_pushed    = Color(41, 255);
    m_button_gradient_bot_pushed    = Color(29, 255);

    m_window_fill_unfocused         = Color(43, 230);
    m_window_fill_focused           = Color(45, 230);
    m_window_title_unfocused        = Color(220, 160);
    m_window_title_focused          = Color(255, 190);
    m_window_header_gradient_top    = Color(74, 255);
    m_window_header_gradient_bot    = Color(58, 255);
    m_window_header_sep_top         = m_border_light;
    m_window_header_sep_bot         = m_border_dark;
    m_window_popup                  = Color(50, 255);
    m_window_popup_transparent      = Color(50, 0);

    m_split_divider                 = Color(90, 255);

    m_menubar_fill                  = Color(32, 255);
    // Flat selection: solid mid-gray-blue, not a glossy button fill
    m_menubar_button_focused        = Color(55, 95, 180, 255);
    m_menu_popup_fill               = Color(42, 255);
    m_menu_separator                = Color(70, 255);

    m_text_box_bg_top               = Color(255, 32);
    m_text_box_bg_bot               = Color(32, 32);
    m_text_box_focused_bg_top       = Color(150, 32);
    m_text_box_focused_bg_bot       = Color(32, 32);
}

void Theme::apply_light_palette() {
    m_drop_shadow                   = Color(0, 80);
    m_transparent                   = Color(0, 0);
    m_border_dark                   = Color(160, 255);
    m_border_light                  = Color(220, 255);
    m_border_medium                 = Color(190, 255);

    m_text_color                    = Color(33, 37, 41, 255);
    m_disabled_text_color           = Color(150, 150, 155, 255);
    m_text_color_shadow             = Color(255, 0); // no shadow on light
    m_text_secondary                = Color(73, 80, 87, 255);
    m_text_heading                  = Color(52, 144, 220, 255);
    m_text_muted                    = Color(108, 117, 125, 255);
    m_text_accent                   = Color(52, 144, 220, 255);
    m_icon_color                    = Color(80, 130, 210, 255);
    m_disabled_icon_color           = Color(160, 160, 170, 255);

    m_success_color                 = Color(40, 167, 69, 255);
    m_danger_color                  = Color(220, 53, 69, 255);
    m_fail_color                    = m_danger_color;
    m_proceed_color                 = Color(52, 144, 220, 255);
    m_warning_color                 = Color(255, 193, 7, 255);
    m_accent_color                  = Color(52, 144, 220, 255);
    m_button_text_on_solid          = Color(255, 255);

    m_screen_background             = Color(235, 237, 242, 255);

    // Soft blue-gray buttons on light chrome
    m_button_gradient_top_focused   = Color(70, 150, 230, 255);
    m_button_gradient_bot_focused   = Color(52, 130, 210, 255);
    m_button_gradient_top_unfocused = Color(250, 250, 252, 255);
    m_button_gradient_bot_unfocused = Color(235, 237, 240, 255);
    m_button_gradient_top_pushed    = Color(48, 120, 200, 255);
    m_button_gradient_bot_pushed    = Color(40, 100, 180, 255);

    m_window_fill_unfocused         = Color(248, 249, 250, 255);
    m_window_fill_focused           = Color(255, 255, 255, 255);
    m_window_title_unfocused        = Color(100, 100, 110, 255);
    m_window_title_focused          = Color(30, 30, 35, 255);
    m_window_header_gradient_top    = Color(245, 245, 248, 255);
    m_window_header_gradient_bot    = Color(230, 232, 236, 255);
    m_window_header_sep_top         = m_border_light;
    m_window_header_sep_bot         = m_border_dark;
    m_window_popup                  = Color(255, 255, 255, 255);
    m_window_popup_transparent      = Color(255, 0);

    m_split_divider                 = Color(200, 202, 210, 255);

    m_menubar_fill                  = Color(245, 245, 248, 255);
    // Flat selection: solid system-blue, no 3D bevels
    m_menubar_button_focused        = Color(0, 122, 255, 255);
    m_menu_popup_fill               = Color(255, 255, 255, 255);
    m_menu_separator                = Color(180, 182, 188, 255);
    // Stronger drop so menubar separates from light screen background
    m_drop_shadow                   = Color(0, 70);

    m_text_box_bg_top               = Color(255, 255, 255, 255);
    m_text_box_bg_bot               = Color(245, 246, 248, 255);
    m_text_box_focused_bg_top       = Color(255, 255, 255, 255);
    m_text_box_focused_bg_bot       = Color(240, 246, 255, 255);
}

void Theme::configure_as_menubar() {
    // Flat strip: no window chrome, no pill-shaped menu buttons
    m_window_corner_radius          = 0;
    m_window_header_height          = 0;
    m_window_drop_shadow_size       = 6;
    m_button_corner_radius          = 0;   // flat highlight, not rounded buttons
    m_drop_shadow                   = Color(0, 90);

    // Fill is the menubar strip / popup panel
    m_window_fill_unfocused         = m_menubar_fill;
    m_window_fill_focused           = m_menubar_fill;
    m_window_popup                  = m_menu_popup_fill;

    // Flat ghost chrome until hover/open: no borders, no gradients
    m_border_light                  = m_transparent;
    m_border_dark                   = m_transparent;
    m_border_medium                 = m_transparent;
    m_button_gradient_top_unfocused = m_transparent;
    m_button_gradient_bot_unfocused = m_transparent;
    // Solid (flat) hover/open fill — same top/bot so there is no gradient
    m_button_gradient_top_focused   = m_menubar_button_focused;
    m_button_gradient_bot_focused   = m_menubar_button_focused;
    m_button_gradient_top_pushed    = m_menubar_button_focused;
    m_button_gradient_bot_pushed    = m_menubar_button_focused;
    m_text_color_shadow             = m_transparent;
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
