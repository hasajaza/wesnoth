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

#include "accessibility/test_driver.hpp"

#include "accessibility/accessibility.hpp"
#include "accessibility/gui_access.hpp"
#include "accessibility/keymap.hpp"
#include "accessibility/speech.hpp"

#include "display.hpp"
#include "events.hpp"
#include "game_data.hpp"
#include "replay.hpp"
#include "resources.hpp"

#include <SDL2/SDL_events.h>
#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_timer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace accessibility::test_driver
{
namespace
{
std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

class driver : public events::pump_monitor
{
public:
	driver(const std::string& script, const std::string& report)
		: report_path_(report)
	{
		std::ifstream in(script);
		std::string line;
		while(std::getline(in, line)) {
			if(!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			std::size_t b = line.find_first_not_of(" \t");
			if(b == std::string::npos || line[b] == '#') {
				continue;
			}
			lines_.push_back(line.substr(b));
		}
		results_.push_back("script: " + script + " (" + std::to_string(lines_.size()) + " commands)");
		next_ticks_ = SDL_GetTicks() + 500;
	}

	void spoken(const std::string& text)
	{
		all_.push_back(text);
		for(const std::string& n : never_) {
			if(lower(text).find(n) != std::string::npos) {
				fail("never: \"" + n + "\" was spoken in: " + text);
			}
		}
	}

	void process(events::pump_info&) override
	{
		if(finished_) {
			return;
		}
		uint32_t now = SDL_GetTicks();
		if(waiting_) {
			if(wait_satisfied()) {
				pass(wait_desc_);
				waiting_ = false;
				next_ticks_ = now + 100;
			} else if(now > wait_deadline_) {
				fail(wait_desc_ + " (timed out; last spoken: " + last_spoken(3) + ")");
				waiting_ = false;
				next_ticks_ = now;
			} else {
				if(press_key_ != 0 && now >= next_press_) {
					push_key(press_key_, press_mods_, true);
					push_key(press_key_, press_mods_, false);
					next_press_ = now + 1500;
				}
				return;
			}
			press_key_ = 0;
		}
		if(now < next_ticks_) {
			return;
		}
		if(mods_held_) {
			SDL_SetModState(KMOD_NONE);
			mods_held_ = false;
		}
		// Run commands until one needs time.
		while(pos_ < lines_.size() && !waiting_ && SDL_GetTicks() >= next_ticks_) {
			run(lines_[pos_++]);
		}
		if(pos_ >= lines_.size() && !waiting_) {
			finish();
		}
	}

private:
	void pass(const std::string& what) { results_.push_back("PASS " + what); }

	void fail(const std::string& what)
	{
		results_.push_back("FAIL " + what);
		++failures_;
	}

	std::string since_mark() const
	{
		std::string res;
		for(std::size_t i = mark_; i < all_.size(); ++i) {
			res += all_[i];
			res += '\n';
		}
		return res;
	}

	std::string last_spoken(std::size_t n) const
	{
		std::string res;
		for(std::size_t i = all_.size() > n ? all_.size() - n : 0; i < all_.size(); ++i) {
			res += "[" + all_[i] + "] ";
		}
		return res;
	}

	bool wait_satisfied()
	{
		if(wait_dialog_) {
			return gui::top_window_id() == wait_text_;
		}
		for(std::size_t i = wait_from_; i < all_.size(); ++i) {
			if(lower(all_[i]).find(wait_text_) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	static uint16_t sdl_mods(unsigned mods)
	{
		uint16_t m = 0;
		if(mods & 1) m |= KMOD_LCTRL;
		if(mods & 2) m |= KMOD_LSHIFT;
		if(mods & 4) m |= KMOD_LALT;
		return m;
	}

	static void push_key(int keycode, unsigned mods, bool down)
	{
		SDL_Event ev{};
		ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
		ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
		ev.key.keysym.sym = keycode;
		ev.key.keysym.scancode = SDL_GetScancodeFromKey(keycode);
		uint16_t m = 0;
		if(mods & 1) m |= KMOD_LCTRL;
		if(mods & 2) m |= KMOD_LSHIFT;
		if(mods & 4) m |= KMOD_LALT;
		ev.key.keysym.mod = m;
		SDL_PushEvent(&ev);
	}

	void run(const std::string& line)
	{
		std::string cmd = line.substr(0, line.find(' '));
		std::string arg = line.find(' ') == std::string::npos ? std::string() : line.substr(line.find(' ') + 1);
		log("test: " + line);
		if(cmd == "wait") {
			next_ticks_ = SDL_GetTicks() + std::atoi(arg.c_str());
		} else if(cmd == "key") {
			int code;
			unsigned mods;
			if(!keymap::parse_key(arg, code, mods)) {
				fail("invalid key: " + arg);
				return;
			}
			// Native hotkeys read the global modifier state, so set it like a
			// real keyboard would while the injected key is processed.
			SDL_SetModState(static_cast<SDL_Keymod>(sdl_mods(mods)));
			mods_held_ = mods != 0;
			push_key(code, mods, true);
			// Like SDL, a printable key without Ctrl/Alt also produces text input.
			if(code >= 32 && code < 127 && !(mods & (1 | 4))) {
				char ch = static_cast<char>(code);
				if((mods & 2) && ch >= 'a' && ch <= 'z') {
					ch = static_cast<char>(ch - 'a' + 'A');
				}
				SDL_Event ev{};
				ev.type = SDL_TEXTINPUT;
				ev.text.text[0] = ch;
				ev.text.text[1] = '\0';
				SDL_PushEvent(&ev);
			}
			push_key(code, mods, false);
			// Give the game time to process the key and any animation.
			next_ticks_ = SDL_GetTicks() + 400;
		} else if(cmd == "click") {
			// A left mouse click on the map hex "x y" (1-based, as spoken).
			int x = 0, y = 0;
			std::istringstream is(arg);
			is >> x >> y;
			display* d = display::get_singleton();
			if(!d) {
				fail("click: no game display");
				return;
			}
			const map_location loc(x - 1, y - 1);
			const int px = d->get_location_x(loc) + display::hex_size() / 2;
			const int py = d->get_location_y(loc) + display::hex_size() / 2;
			SDL_Event ev{};
			ev.type = SDL_MOUSEMOTION;
			ev.motion.x = px;
			ev.motion.y = py;
			SDL_PushEvent(&ev);
			for(Uint32 type : {static_cast<Uint32>(SDL_MOUSEBUTTONDOWN), static_cast<Uint32>(SDL_MOUSEBUTTONUP)}) {
				SDL_Event b{};
				b.type = type;
				b.button.button = SDL_BUTTON_LEFT;
				b.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
				b.button.clicks = 1;
				b.button.x = px;
				b.button.y = py;
				SDL_PushEvent(&b);
			}
			next_ticks_ = SDL_GetTicks() + 400;
		} else if(cmd == "text") {
			SDL_Event ev{};
			ev.type = SDL_TEXTINPUT;
			std::snprintf(ev.text.text, sizeof ev.text.text, "%s", arg.c_str());
			SDL_PushEvent(&ev);
			next_ticks_ = SDL_GetTicks() + 300;
		} else if(cmd == "mark") {
			mark_ = all_.size();
		} else if(cmd == "expect") {
			if(lower(since_mark()).find(lower(arg)) != std::string::npos) {
				pass("expect \"" + arg + "\"");
			} else {
				fail("expect \"" + arg + "\" (spoken since mark: " + since_mark() + ")");
			}
		} else if(cmd == "expect-not") {
			if(lower(since_mark()).find(lower(arg)) == std::string::npos) {
				pass("expect-not \"" + arg + "\"");
			} else {
				fail("expect-not \"" + arg + "\" (spoken since mark: " + since_mark() + ")");
			}
		} else if(cmd == "expect-once") {
			// The text is spoken exactly once since the mark (no repetition).
			const std::string hay = lower(since_mark());
			const std::string needle = lower(arg);
			std::size_t count = 0;
			for(std::size_t pos = hay.find(needle); !needle.empty() && pos != std::string::npos; pos = hay.find(needle, pos + 1)) {
				++count;
			}
			if(count == 1) {
				pass("expect-once \"" + arg + "\"");
			} else {
				fail("expect-once \"" + arg + "\" found " + std::to_string(count) + " times (spoken since mark: " + since_mark() + ")");
			}
		} else if(cmd == "never") {
			never_.push_back(lower(arg));
			for(const std::string& t : all_) {
				if(lower(t).find(lower(arg)) != std::string::npos) {
					fail("never: \"" + arg + "\" was already spoken: " + t);
				}
			}
		} else if(cmd == "wait-for" || cmd == "wait-dialog") {
			std::size_t sp = arg.find(' ');
			int ms = std::atoi(arg.substr(0, sp).c_str());
			wait_text_ = sp == std::string::npos ? std::string() : arg.substr(sp + 1);
			wait_dialog_ = cmd == "wait-dialog";
			if(!wait_dialog_) {
				wait_text_ = lower(wait_text_);
			}
			wait_from_ = mark_;
			wait_deadline_ = SDL_GetTicks() + ms;
			wait_desc_ = line;
			waiting_ = true;
		} else if(cmd == "press-until") {
			std::istringstream is(arg);
			int ms = 0;
			std::string key;
			is >> ms >> key;
			std::string text;
			std::getline(is, text);
			text = lower(text.substr(text.find_first_not_of(' ') == std::string::npos ? text.size() : text.find_first_not_of(' ')));
			int code;
			unsigned mods;
			if(!keymap::parse_key(key, code, mods)) {
				fail("invalid key: " + key);
				return;
			}
			press_key_ = code;
			press_mods_ = mods;
			wait_text_ = text;
			wait_dialog_ = false;
			wait_from_ = all_.size();
			wait_deadline_ = SDL_GetTicks() + ms;
			wait_desc_ = line;
			waiting_ = true;
			next_press_ = SDL_GetTicks() + 1500;
		} else if(cmd == "rng-mark") {
			rng_state_ = synced_state();
			rng_marked_ = !rng_state_.empty();
		} else if(cmd == "rng-unchanged") {
			// Synced randomness is only used inside synced actions, which are all
			// recorded in the replay; the scenario RNG belongs to game_data.
			std::string now = synced_state();
			if(!rng_marked_ || now.empty()) {
				fail("rng-unchanged without rng-mark or game state");
			} else if(now == rng_state_) {
				pass("rng-unchanged (" + now + ")");
			} else {
				fail("rng-unchanged: " + rng_state_ + " -> " + now);
			}
		} else if(cmd == "quit") {
			finish();
		} else {
			fail("unknown command: " + line);
		}
	}

	static std::string synced_state()
	{
		if(!resources::gamedata || !resources::recorder) {
			return std::string();
		}
		return "scenario rng calls " + std::to_string(resources::gamedata->rng().get_random_calls())
			+ ", replay commands " + std::to_string(resources::recorder->ncommands());
	}

	void finish()
	{
		if(finished_) {
			return;
		}
		finished_ = true;
		std::ofstream out(report_path_, std::ios::out | std::ios::trunc | std::ios::binary);
		int passes = 0;
		for(const std::string& r : results_) {
			out << r << '\n';
			if(r.compare(0, 4, "PASS") == 0) {
				++passes;
			}
		}
		out << "SPOKEN TEXT (" << all_.size() << " messages):\n";
		for(const std::string& t : all_) {
			out << "  " << t << '\n';
		}
		out << "SUMMARY: " << passes << " passed, " << failures_ << " failed\n";
		out.close();
		log("test finished: " + std::to_string(passes) + " passed, " + std::to_string(failures_) + " failed");
		speech::wait_idle(2000);
		std::fflush(nullptr);
		std::_Exit(failures_ == 0 ? 0 : 1);
	}

	std::vector<std::string> lines_;
	std::size_t pos_ = 0;
	std::string report_path_;
	std::vector<std::string> results_;
	std::vector<std::string> all_;
	std::vector<std::string> never_;
	std::size_t mark_ = 0;
	int failures_ = 0;
	uint32_t next_ticks_ = 0;
	bool waiting_ = false;
	bool wait_dialog_ = false;
	std::string wait_text_;
	std::string wait_desc_;
	std::size_t wait_from_ = 0;
	uint32_t wait_deadline_ = 0;
	std::string rng_state_;
	bool rng_marked_ = false;
	bool finished_ = false;
	bool mods_held_ = false;
	int press_key_ = 0;
	unsigned press_mods_ = 0;
	uint32_t next_press_ = 0;
};

std::unique_ptr<driver> g_driver;

} // anonymous namespace

void start(const std::string& script_path, const std::string& report_path)
{
	g_driver = std::make_unique<driver>(script_path, report_path);
	log("test driver started: " + script_path);
}

void on_spoken(const std::string& text)
{
	if(g_driver) {
		g_driver->spoken(text);
	}
}

bool active()
{
	return g_driver != nullptr;
}

} // namespace accessibility::test_driver
