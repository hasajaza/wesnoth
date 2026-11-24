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

#include "accessibility/accessibility.hpp"

#include "accessibility/keymap.hpp"
#include "accessibility/speech.hpp"
#include "accessibility/version.hpp"

#include "commandline_options.hpp"
#include "filesystem.hpp"
#include "build_info.hpp"
#include "game_version.hpp"
#include "gettext.hpp"
#include "gui/dialogs/simple_item_selector.hpp"
#include "preferences/general.hpp"
#include "sound.hpp"

#include <SDL2/SDL_events.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <memory>
#include <sstream>

namespace accessibility
{
namespace
{
bool g_enabled = false;
settings g_settings;
std::unique_ptr<std::ofstream> g_log;
std::string g_speech_log_path;
bool g_backend_overridden = false;

std::deque<std::string> g_history;
int g_history_pos = -1; // index into g_history while reviewing, -1 = not reviewing
std::string g_last;
std::chrono::steady_clock::time_point g_last_time;

struct dialogue_line
{
	std::string speaker;
	std::string text;
};
std::deque<dialogue_line> g_dialogue;
int g_dialogue_pos = -1;

const std::size_t max_history = 200;

std::string timestamp()
{
	std::time_t t = std::time(nullptr);
	char buf[32];
	std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
	return buf;
}

void load_settings()
{
	settings& s = g_settings;
	s.enabled = preferences::get("access_enabled", false);
	s.speech_backend = preferences::get("access_speech_backend", std::string("auto"));
	s.speech_engine = preferences::get("access_speech_engine", std::string("auto"));
	s.sapi_fallback = preferences::get("access_sapi_fallback", true);
	s.interrupt = preferences::get("access_interrupt", true);
	s.verbosity = preferences::get_as_attribute("access_verbosity").to_int(1);
	s.combat_verbosity = preferences::get_as_attribute("access_combat_verbosity").to_int(1);
	s.enemy_verbosity = preferences::get_as_attribute("access_enemy_verbosity").to_int(2);
	s.terrain_detail = preferences::get_as_attribute("access_terrain_detail").to_int(1);
	s.coordinates = preferences::get("access_coordinates", true);
	s.turn_summary = preferences::get("access_turn_summary", true);
	s.audio_cues = preferences::get("access_audio_cues", true);
	s.debug_log = preferences::get("access_debug_log", false);
	s.key_bindings = preferences::get("access_key_bindings", std::string());
}

void start_speech()
{
	const settings& s = g_settings;
	speech::start(s.speech_backend, g_speech_log_path, s.sapi_fallback, s.speech_engine);
	// Report what was detected once the worker has initialised.
	speech::wait_idle(3000);
	speech::backend_status st = speech::status();
	log("speech backend: " + st.backend + ", library loaded: " + (st.library_loaded ? "yes" : "no")
		+ ", engine: " + (st.engine.empty() ? "unknown" : st.engine)
		+ (st.detail.empty() ? std::string() : ", detail: " + st.detail));
	auto engines = speech::engine_names();
	if(!engines.empty()) {
		log("available speech engines: " + join(engines));
	}
}

void open_log()
{
	try {
		std::string dir = filesystem::get_logs_dir();
		if(dir.empty()) {
			return;
		}
		filesystem::make_directory(dir);
		std::string path = dir + "/wesnoth-access.log";
		if(filesystem::file_exists(path)) {
			filesystem::delete_file(path + ".old");
			filesystem::rename_dir(path, path + ".old");
		}
		g_log = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::trunc | std::ios::binary);
	} catch(...) {
		g_log.reset();
	}
}

} // anonymous namespace

bool enabled()
{
	return g_enabled;
}

settings& prefs()
{
	return g_settings;
}

void save_settings()
{
	const settings& s = g_settings;
	preferences::set("access_enabled", s.enabled);
	if(!g_backend_overridden) {
		preferences::set("access_speech_backend", s.speech_backend);
	}
	preferences::set("access_speech_engine", s.speech_engine);
	preferences::set("access_sapi_fallback", s.sapi_fallback);
	preferences::set("access_interrupt", s.interrupt);
	preferences::set("access_verbosity", s.verbosity);
	preferences::set("access_combat_verbosity", s.combat_verbosity);
	preferences::set("access_enemy_verbosity", s.enemy_verbosity);
	preferences::set("access_terrain_detail", s.terrain_detail);
	preferences::set("access_coordinates", s.coordinates);
	preferences::set("access_turn_summary", s.turn_summary);
	preferences::set("access_audio_cues", s.audio_cues);
	preferences::set("access_debug_log", s.debug_log);
	preferences::set("access_key_bindings", s.key_bindings);
	if(speech::running()) {
		speech::configure(s.sapi_fallback, s.speech_engine);
	}
	keymap::load(s.key_bindings);
}

manager::manager(const commandline_options& cmdline)
{
	load_settings();
	open_log();

	if(cmdline.access_speech) {
		g_settings.speech_backend = *cmdline.access_speech;
		g_backend_overridden = true;
	}
	if(cmdline.access_speech_log) {
		g_speech_log_path = *cmdline.access_speech_log;
	}
	if(cmdline.accessibility) {
		g_settings.enabled = true;
		preferences::set("access_enabled", true);
	}
	if(cmdline.no_accessibility) {
		g_settings.enabled = false;
	}

	keymap::load(g_settings.key_bindings);

	log("Wesnoth Access " ACCESS_VERSION_STRING " started " + timestamp());
	log("base game version: " + game_config::revision);
	log("build: " + game_config::build_arch());
	log(std::string("accessibility enabled: ") + (g_settings.enabled ? "yes" : "no")
		+ (cmdline.accessibility ? " (--accessibility)" : "") + (cmdline.no_accessibility ? " (--no-accessibility)" : ""));

	if(g_settings.enabled) {
		set_enabled(true);
	}
}

manager::~manager()
{
	if(g_enabled) {
		log("shutting down");
	}
	g_enabled = false;
	speech::stop_worker();
	g_log.reset();
}

void set_enabled(bool on)
{
	if(on == g_enabled) {
		return;
	}
	if(on) {
		if(!speech::running()) {
			start_speech();
		}
		g_enabled = true;
		g_settings.enabled = true;
		speak(_("Wesnoth Access ready. Press F2 for help."), msg::important);
	} else {
		speak(_("Accessibility off. Press Control Alt A to turn it on again."), msg::reply);
		speech::wait_idle(500);
		g_enabled = false;
		g_settings.enabled = false;
	}
	preferences::set("access_enabled", g_settings.enabled);
	log(std::string("accessibility ") + (on ? "enabled" : "disabled"));
}

std::string plain_text(const std::string& markup)
{
	std::string out;
	out.reserve(markup.size());
	bool in_tag = false;
	for(std::size_t i = 0; i < markup.size(); ++i) {
		char c = markup[i];
		if(in_tag) {
			if(c == '>') {
				in_tag = false;
				// Tags like <br/> or </p> separate words.
				out += ' ';
			}
			continue;
		}
		if(c == '<') {
			// Only treat as a tag when it looks like one ("<b>", "</span>", "<span ...>").
			std::size_t close = markup.find('>', i);
			if(close != std::string::npos && i + 1 < markup.size()
				&& (std::isalpha(static_cast<unsigned char>(markup[i + 1])) || markup[i + 1] == '/')) {
				in_tag = true;
				continue;
			}
		}
		if(c == '&') {
			std::size_t semi = markup.find(';', i);
			if(semi != std::string::npos && semi - i <= 8) {
				std::string ent = markup.substr(i + 1, semi - i - 1);
				std::string rep;
				if(ent == "amp") rep = "&";
				else if(ent == "lt") rep = "<";
				else if(ent == "gt") rep = ">";
				else if(ent == "quot") rep = "\"";
				else if(ent == "apos") rep = "'";
				else if(ent == "nbsp") rep = " ";
				else if(!ent.empty() && ent[0] == '#') {
					long code = std::strtol(ent.c_str() + (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? 2 : 1), nullptr,
						ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? 16 : 10);
					if(code > 0 && code < 0x80) {
						rep = std::string(1, static_cast<char>(code));
					} else if(code >= 0x80 && code < 0x800) {
						rep += static_cast<char>(0xC0 | (code >> 6));
						rep += static_cast<char>(0x80 | (code & 0x3F));
					} else if(code >= 0x800 && code < 0x10000) {
						rep += static_cast<char>(0xE0 | (code >> 12));
						rep += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
						rep += static_cast<char>(0x80 | (code & 0x3F));
					}
				}
				if(!rep.empty()) {
					out += rep;
					i = semi;
					continue;
				}
			}
		}
		out += c;
	}
	// Collapse whitespace.
	std::string res;
	res.reserve(out.size());
	bool space = false;
	for(char c : out) {
		if(c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			space = true;
		} else {
			if(space && !res.empty()) {
				res += ' ';
			}
			space = false;
			res += c;
		}
	}
	return res;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep)
{
	std::string res;
	for(const std::string& p : parts) {
		if(p.empty()) {
			continue;
		}
		if(!res.empty()) {
			res += sep;
		}
		res += p;
	}
	return res;
}

void speak(const std::string& raw, msg kind)
{
	if(!g_enabled) {
		return;
	}
	std::string text = plain_text(raw);
	if(text.empty()) {
		return;
	}

	auto now = std::chrono::steady_clock::now();
	// Avoid duplicate announcements of the same event fired twice in a row.
	if(kind == msg::event && text == g_last && now - g_last_time < std::chrono::milliseconds(1500)) {
		return;
	}

	bool interrupt = false;
	switch(kind) {
	case msg::nav:
		interrupt = g_settings.interrupt;
		break;
	case msg::reply:
		interrupt = true;
		break;
	case msg::event:
	case msg::important:
		interrupt = false;
		break;
	}

	speech::say(text, interrupt);
	g_last = text;
	g_last_time = now;
	g_history.push_back(text);
	if(g_history.size() > max_history) {
		g_history.pop_front();
	}
	g_history_pos = -1;
	debug("speak: " + text);
}

void repeat_last()
{
	if(!g_enabled) {
		return;
	}
	if(g_last.empty()) {
		speech::say(_("Nothing to repeat."), true);
	} else {
		speech::say(g_last, true);
	}
}

void review_history(int direction)
{
	if(!g_enabled) {
		return;
	}
	if(g_history.empty()) {
		speech::say(_("No messages yet."), true);
		return;
	}
	int n = static_cast<int>(g_history.size());
	if(g_history_pos < 0) {
		g_history_pos = n - 1;
	}
	g_history_pos += direction;
	if(g_history_pos < 0) {
		g_history_pos = 0;
		cue("edge");
	} else if(g_history_pos >= n) {
		g_history_pos = n - 1;
		cue("edge");
	}
	speech::say(std::to_string(g_history_pos + 1) + " of " + std::to_string(n) + ": " + g_history[g_history_pos], true);
}

void add_dialogue(const std::string& speaker, const std::string& text)
{
	std::string t = plain_text(text);
	if(t.empty()) {
		return;
	}
	std::string sp = plain_text(speaker);
	if(!g_dialogue.empty() && g_dialogue.back().speaker == sp && g_dialogue.back().text == t) {
		return;
	}
	g_dialogue.push_back({sp, t});
	if(g_dialogue.size() > max_history) {
		g_dialogue.pop_front();
	}
	g_dialogue_pos = -1;
	debug("dialogue: " + sp + ": " + t);
}

void review_dialogue(int direction)
{
	if(!g_enabled) {
		return;
	}
	if(g_dialogue.empty()) {
		speech::say(_("No dialogue yet."), true);
		return;
	}
	int n = static_cast<int>(g_dialogue.size());
	if(direction == 0 || g_dialogue_pos < 0) {
		g_dialogue_pos = n - 1;
		if(direction < 0) {
			g_dialogue_pos = std::max(0, n - 2);
		}
	} else {
		g_dialogue_pos += direction;
	}
	if(g_dialogue_pos < 0) {
		g_dialogue_pos = 0;
		cue("edge");
	} else if(g_dialogue_pos >= n) {
		g_dialogue_pos = n - 1;
		cue("edge");
	}
	const dialogue_line& l = g_dialogue[g_dialogue_pos];
	std::string text = l.speaker.empty() ? l.text : l.speaker + ": " + l.text;
	speech::say(_("Dialogue") + std::string(" ") + std::to_string(g_dialogue_pos + 1) + " of " + std::to_string(n) + ". " + text, true);
}

std::size_t dialogue_count()
{
	return g_dialogue.size();
}

const std::string& last_spoken()
{
	return g_last;
}

void log(const std::string& line)
{
	if(g_log && g_log->good()) {
		*g_log << line << '\n';
		g_log->flush();
	}
}

void debug(const std::string& line)
{
	if(g_settings.debug_log) {
		log("[debug] " + line);
	}
}

void cue(const std::string& name)
{
	if(!g_enabled || !g_settings.audio_cues) {
		return;
	}
	static const std::pair<const char*, const char*> cues[] = {
		{"edge", "contract.wav"},
		{"select", "select-unit.wav"},
		{"error", "timer.wav"},
		{"ok", "button.wav"},
		{"turn", "bell.wav"},
		{"alert", "receive.wav"},
		{"move", "slider.wav"},
		{"dialog", "expand.wav"},
	};
	for(const auto& c : cues) {
		if(name == c.first) {
			try {
				sound::play_UI_sound(c.second);
			} catch(...) {
			}
			return;
		}
	}
}

bool handle_global_key(const SDL_Event& event)
{
	if(event.type != SDL_KEYDOWN) {
		return false;
	}
	const SDL_Keycode key = event.key.keysym.sym;
	const unsigned mod = event.key.keysym.mod;
	const bool ctrl = mod & KMOD_CTRL;
	const bool alt = mod & KMOD_ALT;
	const bool shift = mod & KMOD_SHIFT;

	// The on/off toggle works even while the layer is disabled.
	if(key == SDLK_a && ctrl && alt) {
		set_enabled(!g_enabled);
		save_settings();
		return true;
	}
	if(!g_enabled) {
		return false;
	}

	if(key == SDLK_LCTRL || key == SDLK_RCTRL) {
		// Control silences speech, as in screen readers (needed for the SAPI fallback).
		speech::silence();
		return false;
	}

	command cmd = keymap::lookup(context::global, event);
	switch(cmd) {
	case command::repeat_last:
		repeat_last();
		return true;
	case command::history_back:
		review_history(-1);
		return true;
	case command::history_forward:
		review_history(1);
		return true;
	case command::dialogue_last:
		review_dialogue(0);
		return true;
	case command::dialogue_back:
		review_dialogue(-1);
		return true;
	case command::dialogue_forward:
		review_dialogue(1);
		return true;
	case command::settings:
		show_settings_dialog();
		return true;
	case command::help:
		show_command_reference();
		return true;
	case command::stop_speech:
		speech::silence();
		return true;
	default:
		break;
	}
	(void)ctrl;
	(void)shift;
	return false;
}

int run_selftest(const std::string& report_path, const commandline_options& cmdline)
{
	std::ofstream report(report_path, std::ios::out | std::ios::trunc | std::ios::binary);
	report << "Wesnoth Access " ACCESS_VERSION_STRING " speech self test\n";
	report << "base game version: " << game_config::revision << "\n";
	report << "build: " << game_config::build_arch() << "\n";
	std::string backend = cmdline.access_speech ? *cmdline.access_speech : "auto";
	std::string log_path = cmdline.access_speech_log ? *cmdline.access_speech_log : std::string();
	report << "requested backend: " << backend << "\n";

	speech::start(backend, log_path, true, "auto");
	bool idle = speech::wait_idle(5000);
	speech::backend_status st = speech::status();
	report << "worker initialised: " << (idle ? "yes" : "no (timeout)") << "\n";
	report << "backend: " << st.backend << "\n";
	report << "library loaded: " << (st.library_loaded ? "yes" : "no") << "\n";
	report << "engine: " << st.engine << "\n";
	if(!st.detail.empty()) {
		report << "detail: " << st.detail << "\n";
	}
	auto engines = speech::engine_names();
	report << "engines known to Universal Speech: " << (engines.empty() ? std::string("(none)") : join(engines)) << "\n";

	auto t0 = std::chrono::steady_clock::now();
	speech::say("Wesnoth Access speech self test. Unicode: caf\xC3\xA9, na\xC3\xAFve, \xC3\x86lfric.", true);
	speech::say("Queued message.", false);
	speech::say("Interrupting message.", true);
	speech::silence();
	bool done = speech::wait_idle(10000);
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	report << "speak/queue/interrupt/stop requests processed: " << (done ? "yes" : "no (timeout)") << " in " << ms << " ms\n";
	st = speech::status();
	report << "engine after speaking: " << st.engine << "\n";
	speech::stop_worker();
	report << "worker stopped cleanly: yes\n";

	bool ok = idle && done;
#ifdef _WIN32
	if(backend == "auto") {
		ok = ok && st.library_loaded;
	}
#endif
	report << "result: " << (ok ? "PASS" : "FAIL") << "\n";
	return ok ? 0 : 1;
}

namespace
{
std::string onoff(bool b)
{
	return b ? _("on") : _("off");
}

std::string level_name(int v, const std::vector<std::string>& names)
{
	if(v < 0 || v >= static_cast<int>(names.size())) {
		return std::to_string(v);
	}
	return names[v];
}
} // namespace

void show_settings_dialog()
{
	int index = 0;
	for(;;) {
		settings& s = g_settings;
		const std::vector<std::string> verb = {_("brief"), _("normal"), _("detailed")};
		const std::vector<std::string> combat = {_("outcome only"), _("every strike")};
		const std::vector<std::string> enemy = {_("silent"), _("summary"), _("every visible action")};
		const std::vector<std::string> terrain = {_("terrain name only"), _("with unit defense and movement cost"), _("full detail")};
		std::vector<std::string> engines = speech::engine_names();

		std::vector<std::string> items {
			_("Speech engine: ") + (s.speech_engine == "auto" ? std::string(_("automatic")) : s.speech_engine),
			_("Use SAPI when no screen reader is running: ") + onoff(s.sapi_fallback),
			_("Navigation interrupts speech: ") + onoff(s.interrupt),
			_("Verbosity: ") + level_name(s.verbosity, verb),
			_("Combat announcements: ") + level_name(s.combat_verbosity, combat),
			_("Enemy turn announcements: ") + level_name(s.enemy_verbosity, enemy),
			_("Terrain information: ") + level_name(s.terrain_detail, terrain),
			_("Announce coordinates: ") + onoff(s.coordinates),
			_("Turn summary at the start of each turn: ") + onoff(s.turn_summary),
			_("Audio cues: ") + onoff(s.audio_cues),
			_("Debug logging: ") + onoff(s.debug_log),
			_("Test speech"),
			_("Turn accessibility off"),
		};

		gui2::dialogs::simple_item_selector dlg(_("Accessibility Settings"),
			_("Select a setting and press Enter to change it. Press Escape to close."), items);
		dlg.set_selected_index(index);
		dlg.set_ok_label(_("Change"));
		dlg.set_cancel_label(_("Close"));
		dlg.show();
		index = dlg.selected_index();
		if(dlg.get_retval() != gui2::retval::OK || index < 0) {
			break;
		}
		std::string confirm;
		switch(index) {
		case 0: {
			std::vector<std::string> cycle {"auto"};
			cycle.insert(cycle.end(), engines.begin(), engines.end());
			if(cycle.size() == 1) {
				cycle = {"auto", "JAWS", "NVDA", "SAPI5"};
			}
			std::size_t i = 0;
			for(; i < cycle.size(); ++i) {
				if(cycle[i] == s.speech_engine) {
					break;
				}
			}
			s.speech_engine = cycle[(i + 1) % cycle.size()];
			confirm = items[0];
			break;
		}
		case 1: s.sapi_fallback = !s.sapi_fallback; break;
		case 2: s.interrupt = !s.interrupt; break;
		case 3: s.verbosity = (s.verbosity + 1) % 3; break;
		case 4: s.combat_verbosity = (s.combat_verbosity + 1) % 2; break;
		case 5: s.enemy_verbosity = (s.enemy_verbosity + 1) % 3; break;
		case 6: s.terrain_detail = (s.terrain_detail + 1) % 3; break;
		case 7: s.coordinates = !s.coordinates; break;
		case 8: s.turn_summary = !s.turn_summary; break;
		case 9: s.audio_cues = !s.audio_cues; break;
		case 10: s.debug_log = !s.debug_log; break;
		case 11:
			speak(_("This is a speech test. Wesnoth Access is working."), msg::reply);
			continue;
		case 12:
			save_settings();
			set_enabled(false);
			save_settings();
			return;
		default: break;
		}
		save_settings();
		preferences::write_preferences();
		log("setting changed: item " + std::to_string(index));
	}
	save_settings();
	preferences::write_preferences();
}

void show_command_reference()
{
	std::vector<std::string> items = keymap::reference_lines();
	gui2::dialogs::simple_item_selector dlg(_("Wesnoth Access Commands"),
		_("Use the up and down arrow keys to read the commands. Press Escape to close."), items);
	dlg.set_single_button(true);
	dlg.set_ok_label(_("Close"));
	dlg.show();
}

} // namespace accessibility
