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

#include "accessibility/keymap.hpp"

#include "accessibility/accessibility.hpp"
#include "gettext.hpp"
#include "serialization/string_utils.hpp"

#include <SDL2/SDL_events.h>
#include <SDL2/SDL_keyboard.h>

#include <algorithm>
#include <cctype>
#include <map>

namespace accessibility::keymap
{
namespace
{
constexpr unsigned M_CTRL = 1, M_SHIFT = 2, M_ALT = 4;

struct binding
{
	command cmd;
	context ctx;
	int key;
	unsigned mods;
};

struct command_info
{
	command cmd;
	const char* id;
	context ctx;
	const char* description;
	const char* default_key;
};

// clang-format off
const command_info infos[] = {
	{command::help,               "help",               context::global, N_("Accessibility command reference"), "F2"},
	{command::settings,           "settings",           context::global, N_("Accessibility settings"), "Shift+F2"},
	{command::repeat_last,        "repeat_last",        context::global, N_("Repeat the last message"), "F12"},
	{command::history_back,       "history_back",       context::global, N_("Previous message in the speech history"), "Shift+F12"},
	{command::history_forward,    "history_forward",    context::global, N_("Next message in the speech history"), "Ctrl+Shift+F12"},
	{command::dialogue_last,      "dialogue_last",      context::global, N_("Repeat the last dialogue or story text"), "F11"},
	{command::dialogue_back,      "dialogue_back",      context::global, N_("Previous dialogue line"), "Shift+F11"},
	{command::dialogue_forward,   "dialogue_forward",   context::global, N_("Next dialogue line"), "Ctrl+Shift+F11"},
	{command::stop_speech,        "stop_speech",        context::global, N_("Stop speech (Control alone also works)"), ""},

	{command::gui_next,           "gui_next",           context::dialog, N_("Next control"), "Tab"},
	{command::gui_prev,           "gui_prev",           context::dialog, N_("Previous control"), "Shift+Tab"},
	{command::gui_read_all,       "gui_read_all",       context::dialog, N_("Read the whole dialog"), "F4"},
	{command::gui_describe,       "gui_describe",       context::dialog, N_("Describe the focused control in detail"), "Shift+F4"},

	{command::move_n,             "move_n",             context::map, N_("Review cursor north"), "W"},
	{command::move_ne,            "move_ne",            context::map, N_("Review cursor north-east"), "E"},
	{command::move_se,            "move_se",            context::map, N_("Review cursor south-east"), "D"},
	{command::move_s,             "move_s",             context::map, N_("Review cursor south"), "S"},
	{command::move_sw,            "move_sw",            context::map, N_("Review cursor south-west"), "A"},
	{command::move_nw,            "move_nw",            context::map, N_("Review cursor north-west"), "Q"},
	{command::jump_n,             "jump_n",             context::map, N_("Jump north until the terrain or a unit changes"), "Shift+W"},
	{command::jump_ne,            "jump_ne",            context::map, N_("Jump north-east until something changes"), "Shift+E"},
	{command::jump_se,            "jump_se",            context::map, N_("Jump south-east until something changes"), "Shift+D"},
	{command::jump_s,             "jump_s",             context::map, N_("Jump south until something changes"), "Shift+S"},
	{command::jump_sw,            "jump_sw",            context::map, N_("Jump south-west until something changes"), "Shift+A"},
	{command::jump_nw,            "jump_nw",            context::map, N_("Jump north-west until something changes"), "Shift+Q"},
	{command::select_action,      "select_action",      context::map, N_("Select the unit under the cursor, or move or attack with the selected unit"), "Enter"},
	{command::cancel,             "cancel",             context::map, N_("Deselect the selected unit (when nothing is selected: the normal Escape menu)"), "Escape"},
	{command::describe_tile,      "describe_tile",      context::map, N_("Describe the tile under the cursor in detail"), "X"},
	{command::describe_route,     "describe_route",     context::map, N_("Describe the route of the selected unit to the cursor"), "Shift+X"},
	{command::describe_unit,      "describe_unit",      context::map, N_("Describe the unit under the cursor"), "V"},
	{command::describe_unit_full, "describe_unit_full", context::map, N_("Full unit details: attacks, resistances, abilities, traits, advancement"), "Shift+V"},
	{command::where_am_i,         "where_am_i",         context::map, N_("Cursor coordinates and direction from the selected unit"), "C"},
	{command::map_info,           "map_info",           context::map, N_("Map size and scenario name"), "Shift+C"},
	{command::adjacent,           "adjacent",           context::map, N_("Describe the six tiles around the cursor"), "Z"},
	{command::goto_selected,      "goto_selected",      context::map, N_("Cursor to the selected unit"), "Home"},
	{command::goto_leader,        "goto_leader",        context::map, N_("Cursor to your leader"), "Shift+Home"},
	{command::goto_previous,      "goto_previous",      context::map, N_("Cursor back to the previous location"), "Backspace"},
	{command::goto_coordinates,   "goto_coordinates",   context::map, N_("Jump to coordinates"), "Ctrl+G"},
	{command::next_unit,          "next_unit",          context::map, N_("Next of your units"), "F3"},
	{command::prev_unit,          "prev_unit",          context::map, N_("Previous of your units"), "Shift+F3"},
	{command::next_active_unit,   "next_active_unit",   context::map, N_("Next of your units that can still move or attack"), "Ctrl+F3"},
	{command::prev_active_unit,   "prev_active_unit",   context::map, N_("Previous of your units that can still move or attack"), "Ctrl+Shift+F3"},
	{command::next_wounded,       "next_wounded",       context::map, N_("Next wounded unit of yours"), "Alt+F3"},
	{command::prev_wounded,       "prev_wounded",       context::map, N_("Previous wounded unit of yours"), "Alt+Shift+F3"},
	{command::next_enemy,         "next_enemy",         context::map, N_("Next visible enemy"), "F4"},
	{command::prev_enemy,         "prev_enemy",         context::map, N_("Previous visible enemy"), "Shift+F4"},
	{command::next_ally,          "next_ally",          context::map, N_("Next allied unit"), "Ctrl+F4"},
	{command::prev_ally,          "prev_ally",          context::map, N_("Previous allied unit"), "Ctrl+Shift+F4"},
	{command::next_village,       "next_village",       context::map, N_("Next known village"), "F5"},
	{command::prev_village,       "prev_village",       context::map, N_("Previous known village"), "Shift+F5"},
	{command::next_label,         "next_label",         context::map, N_("Next labelled location"), "Ctrl+F5"},
	{command::prev_label,         "prev_label",         context::map, N_("Previous labelled location"), "Ctrl+Shift+F5"},
	{command::next_reachable,     "next_reachable",     context::map, N_("Next destination the selected unit can reach"), "F6"},
	{command::prev_reachable,     "prev_reachable",     context::map, N_("Previous reachable destination"), "Shift+F6"},
	{command::next_target,        "next_target",        context::map, N_("Next attack option of the selected unit (then Enter to attack)"), "F7"},
	{command::prev_target,        "prev_target",        context::map, N_("Previous attack option"), "Shift+F7"},
	{command::objectives,         "objectives",         context::map, N_("Scenario objectives"), "F8"},
	{command::economy,            "economy",            context::map, N_("Turn, gold, income, upkeep and villages"), "F9"},
	{command::time_of_day,        "time_of_day",        context::map, N_("Time of day and its effect on your units"), "Shift+F9"},
	{command::unit_counts,        "unit_counts",        context::map, N_("Unit counts: yours, ready to act, wounded, visible enemies"), "Ctrl+F9"},
	{command::set_bookmark_1,     "set_bookmark_1",     context::map, N_("Set bookmark 1 (also Ctrl+2 to Ctrl+9)"), "Ctrl+1"},
	{command::set_bookmark_2,     "set_bookmark_2",     context::map, nullptr, "Ctrl+2"},
	{command::set_bookmark_3,     "set_bookmark_3",     context::map, nullptr, "Ctrl+3"},
	{command::set_bookmark_4,     "set_bookmark_4",     context::map, nullptr, "Ctrl+4"},
	{command::set_bookmark_5,     "set_bookmark_5",     context::map, nullptr, "Ctrl+5"},
	{command::set_bookmark_6,     "set_bookmark_6",     context::map, nullptr, "Ctrl+6"},
	{command::set_bookmark_7,     "set_bookmark_7",     context::map, nullptr, "Ctrl+7"},
	{command::set_bookmark_8,     "set_bookmark_8",     context::map, nullptr, "Ctrl+8"},
	{command::set_bookmark_9,     "set_bookmark_9",     context::map, nullptr, "Ctrl+9"},
	{command::goto_bookmark_1,    "goto_bookmark_1",    context::map, N_("Go to bookmark 1 (also 2 to 9)"), "1"},
	{command::goto_bookmark_2,    "goto_bookmark_2",    context::map, nullptr, "2"},
	{command::goto_bookmark_3,    "goto_bookmark_3",    context::map, nullptr, "3"},
	{command::goto_bookmark_4,    "goto_bookmark_4",    context::map, nullptr, "4"},
	{command::goto_bookmark_5,    "goto_bookmark_5",    context::map, nullptr, "5"},
	{command::goto_bookmark_6,    "goto_bookmark_6",    context::map, nullptr, "6"},
	{command::goto_bookmark_7,    "goto_bookmark_7",    context::map, nullptr, "7"},
	{command::goto_bookmark_8,    "goto_bookmark_8",    context::map, nullptr, "8"},
	{command::goto_bookmark_9,    "goto_bookmark_9",    context::map, nullptr, "9"},
	{command::native_describe_unit, "native_describe_unit", context::map, N_("Open the game's own help page for the unit under the cursor (normally D)"), "Ctrl+Alt+D"},
	{command::native_statistics,  "native_statistics",  context::map, N_("Open the game's statistics dialog (normally S)"), "Ctrl+Alt+S"},
};
// clang-format on

std::vector<binding> g_bindings;

const std::map<std::string, int>& named_keys()
{
	static const std::map<std::string, int> keys {
		{"enter", SDLK_RETURN}, {"return", SDLK_RETURN}, {"escape", SDLK_ESCAPE}, {"esc", SDLK_ESCAPE},
		{"tab", SDLK_TAB}, {"space", SDLK_SPACE}, {"backspace", SDLK_BACKSPACE}, {"delete", SDLK_DELETE},
		{"home", SDLK_HOME}, {"end", SDLK_END}, {"pageup", SDLK_PAGEUP}, {"pagedown", SDLK_PAGEDOWN},
		{"up", SDLK_UP}, {"down", SDLK_DOWN}, {"left", SDLK_LEFT}, {"right", SDLK_RIGHT}, {"insert", SDLK_INSERT},
		{"f1", SDLK_F1}, {"f2", SDLK_F2}, {"f3", SDLK_F3}, {"f4", SDLK_F4}, {"f5", SDLK_F5}, {"f6", SDLK_F6},
		{"f7", SDLK_F7}, {"f8", SDLK_F8}, {"f9", SDLK_F9}, {"f10", SDLK_F10}, {"f11", SDLK_F11}, {"f12", SDLK_F12},
	};
	return keys;
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

unsigned event_mods(const SDL_Event& event)
{
	unsigned m = 0;
	const unsigned mod = event.key.keysym.mod;
	if(mod & KMOD_CTRL) m |= M_CTRL;
	if(mod & KMOD_SHIFT) m |= M_SHIFT;
	if(mod & KMOD_ALT) m |= M_ALT;
	return m;
}

int normalize_key(int key)
{
	if(key == SDLK_KP_ENTER) return SDLK_RETURN;
	return key;
}

const command_info* info_of(command cmd)
{
	for(const auto& i : infos) {
		if(i.cmd == cmd) {
			return &i;
		}
	}
	return nullptr;
}

std::string context_name(context c)
{
	switch(c) {
	case context::global: return _("Everywhere");
	case context::dialog: return _("In menus and dialogs");
	case context::map: return _("On the game map");
	}
	return std::string();
}

} // anonymous namespace

bool parse_key(const std::string& text, int& keycode, unsigned& mods)
{
	mods = 0;
	keycode = SDLK_UNKNOWN;
	std::vector<std::string> parts = utils::split(text, '+', utils::STRIP_SPACES);
	// "Ctrl++" style: a trailing empty part means the '+' key.
	if(!text.empty() && text.back() == '+' && (parts.empty() || !parts.back().empty())) {
		parts.push_back("+");
	}
	if(parts.empty()) {
		return false;
	}
	for(std::size_t i = 0; i + 1 < parts.size(); ++i) {
		std::string m = lower(parts[i]);
		if(m == "ctrl" || m == "control") mods |= M_CTRL;
		else if(m == "shift") mods |= M_SHIFT;
		else if(m == "alt") mods |= M_ALT;
		else return false;
	}
	std::string k = lower(parts.back());
	auto it = named_keys().find(k);
	if(it != named_keys().end()) {
		keycode = it->second;
		return true;
	}
	if(k.size() == 1) {
		keycode = static_cast<unsigned char>(k[0]);
		return true;
	}
	SDL_Keycode sdl = SDL_GetKeyFromName(parts.back().c_str());
	if(sdl != SDLK_UNKNOWN) {
		keycode = sdl;
		return true;
	}
	return false;
}

void load(const std::string& overrides)
{
	g_bindings.clear();
	std::map<std::string, std::string> custom;
	for(const std::string& entry : utils::split(overrides, ';')) {
		auto eq = entry.find('=');
		if(eq == std::string::npos) {
			continue;
		}
		auto trim = [](std::string v) {
			const char* ws = " \t\r\n";
			std::size_t b = v.find_first_not_of(ws);
			std::size_t e = v.find_last_not_of(ws);
			return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
		};
		std::string id = trim(entry.substr(0, eq));
		std::string key = trim(entry.substr(eq + 1));
		custom[id] = key;
	}
	for(const auto& i : infos) {
		std::string key = i.default_key;
		auto c = custom.find(i.id);
		if(c != custom.end()) {
			key = c->second;
		}
		// A custom binding may list several keys separated by '|'.
		for(const std::string& k : utils::split(key, '|')) {
			int code;
			unsigned mods;
			if(parse_key(k, code, mods)) {
				g_bindings.push_back({i.cmd, i.ctx, code, mods});
			} else if(!k.empty()) {
				log("invalid key binding ignored: " + std::string(i.id) + "=" + k);
			}
		}
	}
}

command lookup(context ctx, const SDL_Event& event)
{
	if(event.type != SDL_KEYDOWN) {
		return command::none;
	}
	if(g_bindings.empty()) {
		load(std::string());
	}
	const int key = normalize_key(event.key.keysym.sym);
	const unsigned mods = event_mods(event);
	for(const binding& b : g_bindings) {
		if(b.ctx == ctx && b.key == key && b.mods == mods) {
			return b.cmd;
		}
	}
	return command::none;
}

std::string key_name(command cmd)
{
	for(const binding& b : g_bindings) {
		if(b.cmd != cmd) {
			continue;
		}
		std::string res;
		if(b.mods & M_CTRL) res += "Ctrl+";
		if(b.mods & M_ALT) res += "Alt+";
		if(b.mods & M_SHIFT) res += "Shift+";
		std::string name = SDL_GetKeyName(b.key);
		if(b.key == SDLK_RETURN) name = "Enter";
		res += name.empty() ? std::string("?") : name;
		return res;
	}
	return _("unassigned");
}

std::string command_id(command cmd)
{
	const command_info* i = info_of(cmd);
	return i ? i->id : std::string();
}

std::vector<std::string> reference_lines()
{
	if(g_bindings.empty()) {
		load(std::string());
	}
	std::vector<std::string> lines;
	for(context ctx : {context::global, context::dialog, context::map}) {
		lines.push_back(context_name(ctx) + ":");
		for(const auto& i : infos) {
			if(i.ctx != ctx || !i.description) {
				continue;
			}
			lines.push_back(key_name(i.cmd) + ": " + translation::gettext(i.description));
		}
		if(ctx == context::dialog) {
			lines.push_back(_("Arrow keys, Home, End, Page Up and Page Down: move inside lists, sliders, menus and text"));
			lines.push_back(_("Enter or Space: press the focused button or toggle the focused check box"));
			lines.push_back(_("Escape: close the dialog"));
		}
		if(ctx == context::map) {
			lines.push_back(_("All other keys keep their normal Wesnoth function, for example N next unit, Space end unit turn, Control Space end turn, U undo, R redo, Control R recruit, Alt R recall, Control S save, Control O load, Control J objectives dialog."));
		}
	}
	lines.push_back(_("Toggle accessibility on or off: Ctrl+Alt+A"));
	return lines;
}

} // namespace accessibility::keymap
