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
 * Key bindings of the accessibility layer.
 *
 * Bindings are context sensitive (global, dialog, map) and can be changed
 * with the preference "access_key_bindings", a list of
 * "command_id=Key;command_id=Ctrl+Shift+Key" entries.
 */

#pragma once

#include <string>
#include <vector>

union SDL_Event;

namespace accessibility
{

enum class context { global, dialog, map };

enum class command
{
	none,
	// global
	repeat_last, history_back, history_forward,
	dialogue_last, dialogue_back, dialogue_forward,
	help, settings, stop_speech,
	// dialogs
	gui_next, gui_prev, gui_read_all, gui_describe,
	// map: review cursor
	move_n, move_ne, move_se, move_s, move_sw, move_nw,
	jump_n, jump_ne, jump_se, jump_s, jump_sw, jump_nw,
	select_action, approach, cancel,
	describe_tile, describe_route, describe_unit, describe_unit_full, where_am_i, map_info,
	adjacent, goto_selected, goto_leader, goto_previous, goto_coordinates,
	next_unit, prev_unit, next_active_unit, prev_active_unit, next_wounded, prev_wounded,
	next_ally, prev_ally, next_enemy, prev_enemy,
	next_village, prev_village, next_label, prev_label,
	next_reachable, prev_reachable, next_target, prev_target, next_keep, prev_keep,
	recruit, recall,
	objectives, economy, time_of_day, unit_counts,
	set_bookmark_1, set_bookmark_2, set_bookmark_3, set_bookmark_4, set_bookmark_5,
	set_bookmark_6, set_bookmark_7, set_bookmark_8, set_bookmark_9,
	goto_bookmark_1, goto_bookmark_2, goto_bookmark_3, goto_bookmark_4, goto_bookmark_5,
	goto_bookmark_6, goto_bookmark_7, goto_bookmark_8, goto_bookmark_9,
	native_describe_unit, native_statistics,
	count_
};

namespace keymap
{

/** Loads the default bindings and applies the user overrides. */
void load(const std::string& overrides);

/** Finds the command bound to the key in the given context (or global). */
command lookup(context ctx, const SDL_Event& event);

/** Human readable key name of the first binding of a command ("Shift+F3"). */
std::string key_name(command cmd);

/** Stable identifier of a command ("next_unit"). */
std::string command_id(command cmd);

/** One line per command: "Key: description", grouped by context. */
std::vector<std::string> reference_lines();

/** Parses a key description ("Ctrl+Shift+F3"); returns false if invalid. Exposed for tests. */
bool parse_key(const std::string& text, int& keycode, unsigned& mods);

} // namespace keymap
} // namespace accessibility
