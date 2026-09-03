/*
    nanogui/spinner.h -- Busy-feedback widgets: overlay spinner and
    indeterminate progress bar

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/widget.h>
#include <nanogui/progressbar.h>

NAMESPACE_BEGIN(nanogui)

/**
 * \class Spinner spinner.h nanogui/spinner.h
 *
 * \brief Overlay widget showing a rotating arc over another widget.
 *
 * Meant to be placed on top of an existing widget (e.g. anchored to the
 * same layout cell as a TextEditor).  While active it dims the covered
 * widget, draws a rotating arc with an optional message, and swallows all
 * mouse input so the underlying widget cannot be edited.  \ref start()
 * registers a looping animation so the screen redraws it live (cached
 * parents cannot freeze a single frame).
 */
class NANOGUI_EXPORT Spinner : public Widget {
public:
    Spinner(Widget *parent, const std::string &message = "");

    const std::string &message() const { return m_message; }
    void set_message(const std::string &m) { m_message = m; }

    bool spinning() const { return m_spinning; }
    void start();
    void stop();

    /* Swallow input while active so the covered widget can't be edited
     * (children are visited back-to-front, and the spinner is added last). */
    virtual bool mouse_button_event(const Vector2i &, int, bool,
                                    int) override { return true; }
    virtual bool mouse_motion_event(const Vector2i &, const Vector2i &,
                                    int, int) override { return true; }
    virtual bool scroll_event(const Vector2i &,
                              const Vector2f &) override { return true; }

    virtual Vector2i preferred_size(NVGcontext *) const override;
    virtual void draw(NVGcontext *ctx) override;

protected:
    std::string m_message;
    bool   m_spinning = false;
    double m_t0 = 0.0;
};

/**
 * \class IndeterminateBar spinner.h nanogui/spinner.h
 *
 * \brief ProgressBar that shows a sliding segment when the total amount
 *        of work is unknown.
 *
 * Call \ref start() / \ref stop() (or \ref set_self_animating) so the
 * widget is drawn live each frame via the screen animation registry.
 */
class NANOGUI_EXPORT IndeterminateBar : public ProgressBar {
public:
    IndeterminateBar(Widget *parent);

    /// Unknown total: ping-pong slider. Known total: call \ref set_progress.
    void start();
    void stop();
    /// Determinate fill in [0, 1]. Stays live so cached parents keep updating.
    void set_progress(float v);
    bool running() const { return visible(); }
    bool determinate() const { return m_determinate; }

    virtual void draw(NVGcontext *ctx) override;

protected:
    bool m_determinate = false;
};

NAMESPACE_END(nanogui)
