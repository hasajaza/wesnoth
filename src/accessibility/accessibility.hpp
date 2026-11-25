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
 * Wesnoth Access: core of the accessibility layer.
 *
 * The accessibility layer is purely an alternative input and information
 * presentation system. It never changes game rules or state: every game
 * action goes through the engine's original code paths (mouse_handler,
 * GUI2 widgets), and every query only reads public, visibility-filtered
 * information.
 */

#pragma once

#include <string>
#include <vector>

class commandline_options;
union SDL_Event;

namespace accessibility
{

/** Persistent accessibility preferences (stored in the normal preferences file). */
struct settings
{
	bool enabled = false;
	std::string speech_backend = "auto";
	std::string speech_engine = "auto";
	bool sapi_fallback = true;
	/** Navigation speech interrupts the previous message. */
	bool interrupt = true;
	/** 0 = brief, 1 = normal, 2 = detailed. */
	int verbosity = 1;
	/** 0 = only the outcome of each fight, 1 = every strike. */
	int combat_verbosity = 1;
	/** 0 = silent, 1 = summary at the end of the enemy turn, 2 = every visible action. */
	int enemy_verbosity = 2;
	/** 0 = terrain name only, 1 = with defense/movement for the selected unit, 2 = everything. */
	int terrain_detail = 1;
	bool coordinates = true;
	bool turn_summary = true;
	bool audio_cues = true;
	bool debug_log = false;
	/** Custom key bindings, see keymap.hpp ("command=key;command=key"). */
	std::string key_bindings;
};

/** Message classes, used for prioritisation. */
enum class msg
{
	/** Result of cursor/focus navigation; interrupts (if enabled) and is not queued. */
	nav,
	/** Direct reply to an explicit command; always interrupts. */
	reply,
	/** Game event; queued behind current speech. */
	event,
	/** Important game event (new turn, victory, unit died); queued, never dropped as duplicate. */
	important,
};

/** True when the accessibility layer is active. */
bool enabled();

/** Current settings (mutable; call save_settings() after changing). */
settings& prefs();

/** Writes the settings to the preferences file and applies speech changes. */
void save_settings();

/**
 * RAII object created in do_gameloop() once preferences are loaded.
 * Reads the settings, applies command line overrides, opens the log file
 * and starts the speech worker if accessibility is enabled.
 */
class manager
{
public:
	explicit manager(const commandline_options& cmdline);
	~manager();
	manager(const manager&) = delete;
	manager& operator=(const manager&) = delete;
};

/** Turns the layer on or off at runtime (Ctrl+Alt+A). */
void set_enabled(bool on);

/**
 * Speaks a message. Markup is removed. Does nothing when disabled.
 */
void speak(const std::string& text, msg kind = msg::reply);

/** A native command was requested but is not available now (greyed out). */
void command_unavailable(const std::string& description);

/** Repeats the most recently spoken message. */
void repeat_last();

/** Moves through the history of spoken messages (-1 older, +1 newer). */
void review_history(int direction);

/** Records a dialogue line ([message], story text) for later review. */
void add_dialogue(const std::string& speaker, const std::string& text);

/** Moves through the dialogue history (-1 older, +1 newer, 0 = latest). */
void review_dialogue(int direction);

/** Number of entries in the dialogue history (tests). */
std::size_t dialogue_count();

/** The most recent spoken message (tests and repeat). */
const std::string& last_spoken();

/** Writes a line to the accessibility log. */
void log(const std::string& line);

/** Writes a line to the accessibility log only when debug logging is on. */
void debug(const std::string& line);

/** Plays an optional audio cue if enabled. Known names: move, edge, select, error, ok, turn, alert. */
void cue(const std::string& name);

/**
 * Handles keys that work everywhere (repeat, history, settings, help,
 * on/off toggle). Returns true if the key was consumed.
 */
bool handle_global_key(const SDL_Event& event);

/** Runs the speech self test and writes a report. Returns the process exit code. */
int run_selftest(const std::string& report_path, const commandline_options& cmdline);

/** Removes Pango/HTML style markup and decodes entities. */
std::string plain_text(const std::string& markup);

/** Turns short "label: value" pairs into "label value," and expands HP/XP/MP, for speech. */
std::string spoken_form(const std::string& text);

/** Joins the non-empty parts with ", ". */
std::string join(const std::vector<std::string>& parts, const std::string& sep = ", ");

/** Opens the accessibility settings dialog (accessible itself). */
void show_settings_dialog();

/** Opens the context sensitive command reference. */
void show_command_reference();

} // namespace accessibility
