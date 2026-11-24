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
 * End-to-end test driver for Wesnoth Access (automated tests only).
 *
 * Enabled with --access-test-script=<file>. It feeds real SDL keyboard
 * events into the normal event queue at the times given by the script and
 * checks the text the accessibility layer spoke. Script commands, one per line:
 *
 *   wait <ms>                    pause before the next command
 *   key <key>                    press and release a key ("Tab", "Shift+F3", "W")
 *   text <characters>            type text (SDL_TEXTINPUT)
 *   mark                         start a new expectation window
 *   expect <text>                text was spoken since the last mark (case-insensitive)
 *   expect-not <text>            text was not spoken since the last mark
 *   never <text>                 text must never be spoken during the whole test
 *   wait-for <ms> <text>         wait until text is spoken (fails after <ms>)
 *   wait-dialog <ms> <id>        wait until the GUI2 window <id> is on top
 *   press-until <ms> <key> <text> press the key every 1.5 s until text is spoken
 *   rng-mark                     remember the synced random state (scenario RNG, replay)
 *   rng-unchanged                fail if that state changed since rng-mark
 *   quit                         write the report and exit
 *
 * Lines starting with '#' are comments. The report lists PASS/FAIL lines and a
 * summary; the process exits with 0 only if everything passed.
 */

#pragma once

#include <string>

namespace accessibility::test_driver
{

/** Starts the driver (called by accessibility::manager). */
void start(const std::string& script_path, const std::string& report_path);

/** Records a spoken message (called by accessibility::speak). */
void on_spoken(const std::string& text);

/** True while a test script is running (tests force the log backend). */
bool active();

} // namespace accessibility::test_driver
