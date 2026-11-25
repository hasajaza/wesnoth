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

#define GETTEXT_DOMAIN "wesnoth-test"

#include "accessibility/accessibility.hpp"
#include "accessibility/game_access.hpp"
#include "accessibility/keymap.hpp"
#include "accessibility/speech.hpp"
#include "map/location.hpp"

#include <SDL2/SDL_events.h>
#include <SDL2/SDL_keycode.h>

#include <boost/test/unit_test.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

BOOST_AUTO_TEST_SUITE(test_accessibility)

BOOST_AUTO_TEST_CASE(test_plain_text_removes_markup)
{
	using accessibility::plain_text;
	BOOST_CHECK_EQUAL(plain_text("<b>Spearman</b>"), "Spearman");
	BOOST_CHECK_EQUAL(plain_text("<span color='#ff0000'>12</span>/<i>40</i>"), "12 / 40");
	BOOST_CHECK_EQUAL(plain_text("Tom &amp; Jerry &lt;3"), "Tom & Jerry <3");
	BOOST_CHECK_EQUAL(plain_text("a\n\n  b\tc"), "a b c");
	BOOST_CHECK_EQUAL(plain_text("caf&#233;"), "caf\xC3\xA9");
	// A '<' that does not start a tag is kept.
	BOOST_CHECK_EQUAL(plain_text("3 < 5"), "3 < 5");
	BOOST_CHECK_EQUAL(plain_text(""), "");
	BOOST_CHECK_EQUAL(plain_text("Attacking . To attack"), "Attacking. To attack");
	BOOST_CHECK_EQUAL(plain_text("Chaotic: -25% .."), "Chaotic: -25%.");
	BOOST_CHECK_EQUAL(plain_text("we have work to do..."), "we have work to do...");
	BOOST_CHECK_EQUAL(plain_text("Base damage 7 ( <i>spear</i> )"), "Base damage 7 (spear)");
	BOOST_CHECK_EQUAL(plain_text("Flat 40% 1 \xE2\xAC\xA3\xE2\x80\x8B\xE2\xAC\xA3\xE2\x80\x8B"), "Flat 40% 1");
}

BOOST_AUTO_TEST_CASE(test_join_skips_empty_parts)
{
	using accessibility::join;
	BOOST_CHECK_EQUAL(join({"a", "", "b"}), "a, b");
	BOOST_CHECK_EQUAL(join({"", ""}), "");
	BOOST_CHECK_EQUAL(join({"x", "y"}, ". "), "x. y");
}

BOOST_AUTO_TEST_CASE(test_keymap_parse_key)
{
	using accessibility::keymap::parse_key;
	int code = 0;
	unsigned mods = 0;
	BOOST_CHECK(parse_key("W", code, mods));
	BOOST_CHECK_EQUAL(code, static_cast<int>(SDLK_w));
	BOOST_CHECK_EQUAL(mods, 0u);
	BOOST_CHECK(parse_key("Ctrl+Shift+F3", code, mods));
	BOOST_CHECK_EQUAL(code, static_cast<int>(SDLK_F3));
	BOOST_CHECK_EQUAL(mods, 3u);
	BOOST_CHECK(parse_key("Alt+Home", code, mods));
	BOOST_CHECK_EQUAL(code, static_cast<int>(SDLK_HOME));
	BOOST_CHECK_EQUAL(mods, 4u);
	BOOST_CHECK(parse_key("Enter", code, mods));
	BOOST_CHECK_EQUAL(code, static_cast<int>(SDLK_RETURN));
	BOOST_CHECK(!parse_key("Hyper+X", code, mods));
	BOOST_CHECK(!parse_key("", code, mods));
}

namespace
{
SDL_Event key_event(SDL_Keycode key, uint16_t mod)
{
	SDL_Event ev{};
	ev.type = SDL_KEYDOWN;
	ev.key.keysym.sym = key;
	ev.key.keysym.mod = mod;
	return ev;
}
} // namespace

BOOST_AUTO_TEST_CASE(test_keymap_default_bindings)
{
	using namespace accessibility;
	keymap::load("");
	// Six hex directions on W E D S A Q, as documented.
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_w, 0)) == command::move_n);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_e, 0)) == command::move_ne);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_d, 0)) == command::move_se);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_s, 0)) == command::move_s);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_a, 0)) == command::move_sw);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_q, 0)) == command::move_nw);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_w, KMOD_LSHIFT)) == command::jump_n);
	// Ctrl+W keeps its native meaning (not bound in the map context).
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_w, KMOD_LCTRL)) == command::none);
	// Keypad Enter behaves like Enter.
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_KP_ENTER, 0)) == command::select_action);
	BOOST_CHECK(keymap::lookup(context::dialog, key_event(SDLK_TAB, 0)) == command::gui_next);
	BOOST_CHECK(keymap::lookup(context::dialog, key_event(SDLK_TAB, KMOD_RSHIFT)) == command::gui_prev);
	BOOST_CHECK(keymap::lookup(context::global, key_event(SDLK_F12, 0)) == command::repeat_last);
	// Native keys that must stay native: N (next unit), U (undo), Space (end unit turn).
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_n, 0)) == command::none);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_u, 0)) == command::none);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_SPACE, 0)) == command::none);
}

BOOST_AUTO_TEST_CASE(test_keymap_overrides)
{
	using namespace accessibility;
	keymap::load("move_n=Up; move_s = Down|K ;bogus=Ctrl+Q;next_unit=NotAKey+++");
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_UP, 0)) == command::move_n);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_w, 0)) == command::none);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_DOWN, 0)) == command::move_s);
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_k, 0)) == command::move_s);
	keymap::load("");
	BOOST_CHECK(keymap::lookup(context::map, key_event(SDLK_w, 0)) == command::move_n);
	BOOST_CHECK(!keymap::reference_lines().empty());
}

BOOST_AUTO_TEST_CASE(test_hex_directions_and_distances)
{
	using accessibility::game::direction_name;
	using accessibility::game::relative_position;
	// The review cursor uses the engine's own adjacency functions; check the
	// six directions round-trip on both column parities.
	for(int x : {4, 5}) {
		map_location start(x, 6);
		for(int d = 0; d < map_location::NDIRECTIONS; ++d) {
			auto dir = static_cast<map_location::DIRECTION>(d);
			map_location n = start.get_direction(dir);
			BOOST_CHECK_EQUAL(distance_between(start, n), 1u);
			BOOST_CHECK(start.get_relative_dir(n) == dir);
			BOOST_CHECK(n.get_direction(map_location::get_opposite_dir(dir)) == start);
			BOOST_CHECK(!direction_name(dir).empty());
		}
	}
	BOOST_CHECK_EQUAL(relative_position(map_location(2, 2), map_location(2, 5)), "3 hexes south");
	BOOST_CHECK_EQUAL(relative_position(map_location(2, 5), map_location(2, 4)), "1 hex north");
	BOOST_CHECK_EQUAL(relative_position(map_location(3, 3), map_location(3, 3)), "same hex");
}

BOOST_AUTO_TEST_CASE(test_speech_log_backend)
{
	namespace speech = accessibility::speech;
	const std::string path = "access_speech_test.log";
	std::remove(path.c_str());
	speech::start("log", path, true, "auto");
	BOOST_CHECK(speech::running());
	speech::say("first", false);
	speech::say("second \xC3\xA9", true);
	speech::say("", false); // ignored
	BOOST_CHECK(speech::wait_idle(5000));
	speech::silence();
	BOOST_CHECK(speech::wait_idle(5000));
	// silence() also discards utterances that are still waiting in the queue.
	speech::say("third", false);
	speech::silence();
	BOOST_CHECK(speech::wait_idle(5000));
	speech::backend_status st = speech::status();
	BOOST_CHECK_EQUAL(st.backend, "log");
	speech::stop_worker();
	BOOST_CHECK(!speech::running());
	speech::stop_worker(); // twice is harmless

	std::ifstream in(path);
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string out = ss.str();
	// An interrupting message may drop earlier queued ones; the interrupting
	// one and the stop must always arrive, in order.
	BOOST_CHECK(out.find("SAY! second \xC3\xA9") != std::string::npos);
	BOOST_CHECK(out.find("STOP") != std::string::npos);
	BOOST_CHECK(out.find("SAY! second") < out.find("STOP"));
	BOOST_CHECK(out.find("SAY+ first") == std::string::npos || out.find("SAY+ first") < out.find("SAY! second"));
	std::remove(path.c_str());
}

BOOST_AUTO_TEST_CASE(test_speak_is_silent_when_disabled)
{
	// Without the manager the layer is disabled: speak() must not record anything.
	BOOST_CHECK(!accessibility::enabled());
	const std::string before = accessibility::last_spoken();
	accessibility::speak("should not be spoken");
	BOOST_CHECK_EQUAL(accessibility::last_spoken(), before);
}

BOOST_AUTO_TEST_CASE(test_spoken_form_reads_label_value_pairs_without_pauses)
{
	using accessibility::spoken_form;
	BOOST_CHECK_EQUAL(spoken_form("HP: 29 Moves: 6 Cost: 17 Alignment: neutral Required XP: 44"),
		"hit points 29, Moves 6, Cost 17, Alignment neutral, Required experience 44");
	BOOST_CHECK_EQUAL(spoken_form("HP: 39/39 XP: 0/37"), "hit points 39/39, experience 0/37");
	BOOST_CHECK_EQUAL(spoken_form("Lawful units: +25%. Chaotic units: -25%."), "Lawful units +25%. Chaotic units -25%.");
	BOOST_CHECK_EQUAL(spoken_form("Chance to kill Quintain: 82%."), "Chance to kill Quintain 82%.");
	// Colons before ordinary text stay.
	BOOST_CHECK_EQUAL(spoken_form("Note: These dummies only attack"), "Note: These dummies only attack");
	BOOST_CHECK_EQUAL(spoken_form("Time of day: Morning."), "Time of day: Morning.");
	BOOST_CHECK_EQUAL(spoken_form("Traits: strong, quick"), "Traits: strong, quick");
	BOOST_CHECK_EQUAL(spoken_form("Victory: Defeat the orc leader"), "Victory: Defeat the orc leader");
}

BOOST_AUTO_TEST_SUITE_END()
