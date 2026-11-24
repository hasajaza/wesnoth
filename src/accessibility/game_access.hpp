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
 * Accessibility of the game map: the review cursor, information commands and
 * announcements of game events.
 *
 * Game rules are never touched. Actions (select, move, attack) are performed
 * by setting the native hovered hex to the review cursor and calling the
 * native mouse_handler functions, exactly like a mouse click. Information is
 * only read from the game state with the visibility checks of the viewing
 * team (fog, shroud, invisible units), mostly through the native sidebar
 * report generators.
 */

#pragma once

#include "map/location.hpp"

#include <string>
#include <vector>

union SDL_Event;
class play_controller;
class unit;
class team;
struct battle_context_unit_stats;
class controller_base;

namespace accessibility::game
{

/**
 * Called from controller_base::handle_event for key events while no dialog
 * is open. Returns true if the event was consumed.
 */
bool handle_event(controller_base& controller, const SDL_Event& event);

/** The game window regained the input focus while the map is shown. */
void on_focus_gained(controller_base& controller);

/** A play controller starts (scenario start or load). */
void on_controller_start(play_controller& pc);

/** The play controller is destroyed. */
void on_controller_end(play_controller& pc);

/** A human player's turn begins (after the turn dialog). */
void on_human_turn_start(play_controller& pc);

/** Another side (AI) starts or ends its turn. */
void on_other_turn_start(play_controller& pc, int side);
void on_other_turn_end(play_controller& pc, int side);

/** The scenario ended and the map can be reviewed (linger mode). */
void on_linger(play_controller& pc);

/** mouse_handler selected a hex (unit selection or deselection). */
void on_select(const map_location& hex);

/** A unit finished moving (actions/move.cpp). */
void on_move(const unit* u, const map_location& from, const map_location& to, int side, bool captured_village);

/** Combat began / one strike happened / combat ended (actions/attack.cpp). */
void on_attack_start(const unit& attacker, const unit& defender, const battle_context_unit_stats& a, const battle_context_unit_stats& d);
void on_strike(const unit& attacker, const unit& defender, const std::string& weapon, bool hits, int damage,
	bool dies, bool poisons, bool slows, bool petrifies, int drained);
void on_attack_end(const unit* attacker, const unit* defender, int attacker_xp, int defender_xp);

/** A unit was recruited or recalled. */
void on_unit_placed(const unit& u, bool recall);

/** An undo or redo happened (@a done) or was impossible. */
void on_undo_redo(bool done, bool redo);

/** A unit was healed (amount > 0), harmed by poison (amount < 0) and/or cured, at the start of a turn. */
void on_heal(const unit& u, int amount, bool cured);

/** A unit advanced to a new type. */
void on_advance(const unit& u);

/** Floating announcement shown by the display (ambush, units sighted, ...). */
void on_announce(const std::string& text);

/** Text shown as an overlay on the map ([print], scenario hints). */
void on_overlay_text(const std::string& text);

/** Chat or [print]-style message added to the chat area. */
void on_chat(const std::string& speaker, const std::string& text);

/** Pure helpers, exposed for the automated tests. */
std::string direction_name(map_location::DIRECTION dir);
std::string relative_position(const map_location& from, const map_location& to);

} // namespace accessibility::game
