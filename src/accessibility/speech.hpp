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
 * Speech output for Wesnoth Access.
 *
 * All calls into the speech library happen on a dedicated worker thread so
 * that a slow or hung screen reader can never block the game's input or
 * rendering loop. The public functions only push requests into a queue.
 *
 * Backends:
 *  - "auto"      Windows: Universal Speech (JAWS, NVDA, other screen readers,
 *                optional SAPI 5 fallback). Elsewhere: speech-dispatcher's
 *                spd-say if available, otherwise silent.
 *  - "log"       Writes every utterance to a file (automated tests).
 *  - "stdout"    Prints every utterance to standard output.
 *  - "none"      Silent.
 */

#pragma once

#include <deque>
#include <string>
#include <vector>

namespace accessibility::speech
{

/** Information about the active backend, for logging and the settings menu. */
struct backend_status
{
	std::string backend;      /**< Backend id: universal_speech, log, stdout, spd, none */
	bool library_loaded = false;
	std::string engine;       /**< Detected screen reader / engine name, if known. */
	std::string detail;       /**< Free text (errors, paths). */
};

/**
 * Starts the speech worker.
 *
 * @param backend      Backend id (see file description).
 * @param log_path     Output file for the "log" backend.
 * @param sapi_fallback Whether Universal Speech may fall back to SAPI 5 when
 *                     no screen reader is running.
 * @param engine       Universal Speech engine to force ("auto", "jaws", "nvda",
 *                     "sapi" or any engine name Universal Speech reports).
 */
void start(const std::string& backend, const std::string& log_path, bool sapi_fallback, const std::string& engine);

/** Stops the worker, flushing nothing further. Safe to call more than once. */
void stop_worker();

/** True if the worker is running. */
bool running();

/**
 * Queues text for speaking.
 *
 * @param text       UTF-8 text. Empty strings are ignored.
 * @param interrupt  If true, current speech and pending queued messages are
 *                   discarded first.
 */
void say(const std::string& text, bool interrupt);

/** Silences speech immediately and clears the queue. */
void silence();

/** Changes the SAPI fallback / forced engine at runtime. */
void configure(bool sapi_fallback, const std::string& engine);

/** Returns a snapshot of the backend status (thread safe). */
backend_status status();

/**
 * Blocks until all queued requests were handed to the backend or the timeout
 * (milliseconds) expired. Only used by the self test and the automated tests.
 */
bool wait_idle(unsigned timeout_ms);

/** The list of engines Universal Speech knows (empty on other platforms). */
std::vector<std::string> engine_names();

} // namespace accessibility::speech
