/*
	Copyright (C) 2026 by the Wesnoth Access contributors
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

/**
 * @file
 * Generic accessibility for GUI2 dialogs.
 *
 * Works for every GUI2 window, including the ones generated from WML or Lua
 * at runtime, by walking the widget tree:
 *  - Tab / Shift+Tab move an accessibility focus over the interactive widgets
 *    (buttons, check boxes, lists, trees, sliders, menus, text fields, long
 *    texts). The native GUI2 keyboard focus follows, so lists, sliders and
 *    text fields keep their own keyboard handling.
 *  - Enter / Space press buttons and toggle check boxes through the normal
 *    GUI2 click signal, so every native callback runs.
 *  - After each key the state of all controls is compared with the state
 *    before the key, and changes (new list selection, slider value, text
 *    cursor...) are spoken.
 *  - Opening a window announces its title, its main text and the focused
 *    control. F4 reads everything.
 */

#pragma once

#include <string>

union SDL_Event;

namespace gui2
{
class window;
class widget;
}

namespace accessibility::gui
{

/** Called by gui2::window::show() after the first draw. */
void window_opened(gui2::window& w);

/** Called by gui2::window::show() once its loop ended. */
void window_closed(gui2::window& w);

/** Called once per iteration of the modal loop; detects asynchronous text changes. */
void window_tick(gui2::window& w);

/**
 * Called before GUI2 dispatches a key event (SDL_KEYDOWN or SDL_TEXTINPUT).
 * Returns true if the event was consumed by the accessibility layer.
 */
bool key_before(gui2::window& w, const SDL_Event& event);

/** Called after GUI2 dispatched a key event the layer did not consume. */
void key_after(gui2::window& w, const SDL_Event& event);

/** Called when the game window regains the input focus. */
void focus_regained();

/** Describes a single widget (name, role, state, value). Exposed for tests. */
std::string describe_widget(gui2::widget& w);

/** Full text of the window: title, texts and controls. Exposed for tests. */
std::string read_all_text(gui2::window& w);

/** True if a modal GUI2 window is currently being shown. */
bool dialog_active();

/** Id of the topmost modal GUI2 window, empty if none (tests). */
std::string top_window_id();

/** Number of modal dialogs currently open (help reader uses it to step aside for newer dialogs). */
std::size_t stack_depth();

} // namespace accessibility::gui
