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
 * Keyboard reader for the game's help system (F1, unit encyclopedia,
 * unit and terrain descriptions).
 *
 * The 1.18 help browser is a legacy (non-GUI2) widget whose topic list only
 * reacts while the mouse pointer is over it. This reader works on the same
 * help data (sections, topics, cross-references), speaks it, and keeps the
 * visual browser showing the same topic.
 *
 * Topic list: Up/Down move, Right or Enter open a section or a topic,
 * Left closes a section or goes to the parent, letters jump to the next
 * entry starting with that letter.
 * Reading a topic: Up/Down previous/next paragraph, Home/End first/last,
 * Tab/Shift+Tab next/previous link, Enter follows the selected link or
 * reads the whole topic, Backspace goes back, Left returns to the list.
 * Escape closes the help.
 */

#pragma once

#include "events.hpp"

#include <memory>
#include <string>
#include <vector>

namespace help
{
struct section;
struct topic;
class help_browser;
}

namespace accessibility
{

class help_reader : public events::sdl_handler
{
public:
	help_reader(const help::section& toplevel, help::help_browser& browser, const std::string& start_topic);
	~help_reader() override;

	void handle_event(const SDL_Event& event) override;

	/** Announces the help again (window focus regained). */
	void refocus();

	/** Number of GUI dialogs that were open when the help started. */
	std::size_t dialog_depth() const { return depth_; }

	/** True once the user closed the help with Escape. */
	bool closed() const { return closed_; }

	/** Converts a topic to paragraphs and links (exposed for tests). */
	static void topic_to_text(const help::topic& t, std::vector<std::string>& paragraphs,
		std::vector<std::pair<std::string, std::string>>& links);

private:
	struct item
	{
		const help::section* sec;
		const help::topic* top;
		int level;
	};

	void rebuild();
	void say_item(bool with_position);
	void open_topic(const help::topic& t, bool from_link);
	void say_paragraph();
	bool handle_list_key(int key, unsigned mod);
	bool handle_reading_key(int key, unsigned mod);
	const help::topic* find(const std::string& id) const;
	bool expanded(const help::section* s) const;

	const help::section& toplevel_;
	help::help_browser& browser_;
	std::vector<item> items_;
	std::vector<const help::section*> expanded_;
	std::size_t pos_ = 0;
	bool reading_ = false;
	const help::topic* topic_ = nullptr;
	std::vector<std::string> paragraphs_;
	std::vector<std::pair<std::string, std::string>> links_; /**< text, topic id */
	std::size_t para_ = 0;
	int link_ = -1;
	std::vector<const help::topic*> history_;
	bool closed_ = false;
	bool suppress_text_ = false;
	std::size_t depth_ = 0;
};

/**
 * Called when the game window gets the focus back: if a help reader is open
 * and no newer dialog is above it (depth = open dialogs), it announces itself
 * and true is returned.
 */
bool help_reader_refocus(std::size_t depth);

} // namespace accessibility
