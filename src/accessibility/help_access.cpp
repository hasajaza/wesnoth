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

#include "accessibility/help_access.hpp"

#include "accessibility/accessibility.hpp"

#include "config.hpp"
#include "gettext.hpp"
#include "help/help_browser.hpp"
#include "help/help_impl.hpp"
#include "serialization/parser.hpp"

#include <SDL2/SDL_events.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>

namespace accessibility
{
namespace
{
std::string clean_title(const std::string& t)
{
	return plain_text(t);
}

/** Splits text into paragraphs at blank lines and line breaks. */
void add_text(std::string& current, std::vector<std::string>& paragraphs, const std::string& text)
{
	for(char c : text) {
		if(c == '\n') {
			std::string p = plain_text(current);
			if(!p.empty()) {
				paragraphs.push_back(p);
			}
			current.clear();
		} else {
			current += c;
		}
	}
}

void flush(std::string& current, std::vector<std::string>& paragraphs)
{
	std::string p = plain_text(current);
	if(!p.empty()) {
		paragraphs.push_back(p);
	}
	current.clear();
}
} // namespace

void help_reader::topic_to_text(const help::topic& t, std::vector<std::string>& paragraphs,
	std::vector<std::pair<std::string, std::string>>& links)
{
	paragraphs.clear();
	links.clear();
	std::string current;
	for(const std::string& item : t.text.parsed_text()) {
		if(!item.empty() && item[0] == '[') {
			config cfg;
			try {
				std::istringstream stream(item);
				read(cfg, stream);
			} catch(...) {
				continue;
			}
			if(auto ref = cfg.optional_child("ref")) {
				std::string text = (*ref)["text"].str();
				std::string dst = (*ref)["dst"].str();
				current += text;
				if(!dst.empty()) {
					links.emplace_back(plain_text(text), dst);
				}
			} else if(auto header = cfg.optional_child("header")) {
				flush(current, paragraphs);
				current = (*header)["text"].str();
				flush(current, paragraphs);
			} else if(auto bold = cfg.optional_child("bold")) {
				current += (*bold)["text"].str();
			} else if(auto italic = cfg.optional_child("italic")) {
				current += (*italic)["text"].str();
			} else if(auto format = cfg.optional_child("format")) {
				add_text(current, paragraphs, (*format)["text"].str());
			} else if(cfg.has_child("jump")) {
				current += ' ';
			}
			// [img] carries no text.
		} else {
			add_text(current, paragraphs, item);
		}
	}
	flush(current, paragraphs);
}

help_reader::help_reader(const help::section& toplevel, help::help_browser& browser, const std::string& start_topic)
	: events::sdl_handler(true)
	, toplevel_(toplevel)
	, browser_(browser)
{
	rebuild();
	const help::topic* t = find(start_topic);
	std::string intro = _("Help. Up and Down move, Enter opens, Escape closes.");
	if(t && start_topic != help::default_show_topic) {
		speak(intro, msg::reply);
		open_topic(*t, false);
	} else {
		speak(intro + " " + _("Topics") + ".", msg::reply);
		say_item(true);
	}
	log("help reader opened" + (t ? ": " + t->id : std::string()));
}

help_reader::~help_reader()
{
	log("help reader closed");
}

const help::topic* help_reader::find(const std::string& id) const
{
	if(id.empty()) {
		return nullptr;
	}
	if(const help::topic* t = help::find_topic(toplevel_, id)) {
		return t;
	}
	return help::find_topic(help::hidden_sections, id);
}

bool help_reader::expanded(const help::section* s) const
{
	return std::find(expanded_.begin(), expanded_.end(), s) != expanded_.end();
}

void help_reader::rebuild()
{
	const void* current = nullptr;
	if(pos_ < items_.size()) {
		current = items_[pos_].sec ? static_cast<const void*>(items_[pos_].sec) : static_cast<const void*>(items_[pos_].top);
	}
	items_.clear();
	std::function<void(const help::section&, int)> add = [&](const help::section& s, int level) {
		for(const help::section& sub : s.sections) {
			items_.push_back({&sub, nullptr, level});
			if(expanded(&sub)) {
				add(sub, level + 1);
			}
		}
		for(const help::topic& t : s.topics) {
			// Topics whose id starts with '.' are hidden in the native menu too.
			if(!t.id.empty() && t.id[0] == '.') {
				continue;
			}
			items_.push_back({nullptr, &t, level});
		}
	};
	add(toplevel_, 0);
	pos_ = 0;
	for(std::size_t i = 0; i < items_.size(); ++i) {
		const void* p = items_[i].sec ? static_cast<const void*>(items_[i].sec) : static_cast<const void*>(items_[i].top);
		if(p == current) {
			pos_ = i;
		}
	}
}

void help_reader::say_item(bool with_position)
{
	if(items_.empty()) {
		speak(_("No help topics."), msg::nav);
		return;
	}
	const item& it = items_[pos_];
	std::string text;
	if(it.sec) {
		text = clean_title(it.sec->title) + ", " + _("section") + ", "
			+ (expanded(it.sec) ? _("open") : _("closed"));
	} else {
		text = clean_title(it.top->title);
	}
	if(it.level > 0) {
		text += ", " + std::string(_("level")) + " " + std::to_string(it.level + 1);
	}
	if(with_position) {
		text += ", " + std::to_string(pos_ + 1) + " " + _("of") + " " + std::to_string(items_.size());
	}
	speak(text, msg::nav);
}

void help_reader::open_topic(const help::topic& t, bool from_link)
{
	if(topic_ && from_link) {
		history_.push_back(topic_);
	}
	topic_ = &t;
	reading_ = true;
	topic_to_text(t, paragraphs_, links_);
	para_ = 0;
	link_ = -1;
	try {
		browser_.show_topic(t.id);
	} catch(...) {
	}
	std::string text = clean_title(t.title);
	if(!links_.empty()) {
		text += ". " + std::to_string(links_.size()) + " " + _("links, Tab to select");
	}
	if(!paragraphs_.empty()) {
		text += ". " + paragraphs_[0];
	}
	speak(text, msg::reply);
}

void help_reader::say_paragraph()
{
	if(paragraphs_.empty()) {
		speak(_("This topic has no text."), msg::nav);
		return;
	}
	speak(paragraphs_[para_], msg::nav);
}

bool help_reader::handle_list_key(int key, unsigned mod)
{
	if(items_.empty()) {
		return false;
	}
	const bool plain = !(mod & (KMOD_CTRL | KMOD_ALT));
	switch(key) {
	case SDLK_DOWN:
		if(pos_ + 1 < items_.size()) ++pos_; else cue("edge");
		say_item(true);
		return true;
	case SDLK_UP:
		if(pos_ > 0) --pos_; else cue("edge");
		say_item(true);
		return true;
	case SDLK_HOME:
		pos_ = 0;
		say_item(true);
		return true;
	case SDLK_END:
		pos_ = items_.size() - 1;
		say_item(true);
		return true;
	case SDLK_RIGHT:
	case SDLK_RETURN:
	case SDLK_KP_ENTER: {
		const item it = items_[pos_];
		if(it.sec) {
			if(expanded(it.sec)) {
				if(key != SDLK_RIGHT) {
					expanded_.erase(std::find(expanded_.begin(), expanded_.end(), it.sec));
				}
			} else {
				expanded_.push_back(it.sec);
			}
			rebuild();
			say_item(true);
		} else {
			history_.clear();
			open_topic(*it.top, false);
		}
		return true;
	}
	case SDLK_LEFT: {
		const item it = items_[pos_];
		if(it.sec && expanded(it.sec)) {
			expanded_.erase(std::find(expanded_.begin(), expanded_.end(), it.sec));
			rebuild();
			say_item(true);
			return true;
		}
		// Go to the parent section.
		for(std::size_t i = pos_; i-- > 0;) {
			if(items_[i].sec && items_[i].level < it.level) {
				pos_ = i;
				break;
			}
		}
		say_item(true);
		return true;
	}
	default:
		break;
	}
	if(plain && key >= SDLK_a && key <= SDLK_z) {
		// First-letter navigation.
		const char wanted = static_cast<char>(key);
		for(std::size_t step = 1; step <= items_.size(); ++step) {
			std::size_t i = (pos_ + step) % items_.size();
			std::string title = clean_title(items_[i].sec ? items_[i].sec->title : items_[i].top->title);
			if(!title.empty() && std::tolower(static_cast<unsigned char>(title[0])) == wanted) {
				pos_ = i;
				say_item(true);
				return true;
			}
		}
		cue("error");
		return true;
	}
	return false;
}

bool help_reader::handle_reading_key(int key, unsigned mod)
{
	const bool shift = mod & KMOD_SHIFT;
	switch(key) {
	case SDLK_DOWN:
		if(para_ + 1 < paragraphs_.size()) ++para_; else cue("edge");
		link_ = -1;
		say_paragraph();
		return true;
	case SDLK_UP:
		if(para_ > 0) --para_; else cue("edge");
		link_ = -1;
		say_paragraph();
		return true;
	case SDLK_HOME:
		para_ = 0;
		say_paragraph();
		return true;
	case SDLK_END:
		para_ = paragraphs_.empty() ? 0 : paragraphs_.size() - 1;
		say_paragraph();
		return true;
	case SDLK_TAB:
		if(links_.empty()) {
			speak(_("No links in this topic."), msg::nav);
			return true;
		}
		if(shift) {
			link_ = link_ <= 0 ? static_cast<int>(links_.size()) - 1 : link_ - 1;
		} else {
			link_ = (link_ + 1) % static_cast<int>(links_.size());
		}
		speak(_("link") + std::string(" ") + links_[link_].first + ", " + std::to_string(link_ + 1) + " "
			+ _("of") + " " + std::to_string(links_.size()), msg::nav);
		return true;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		if(link_ >= 0 && link_ < static_cast<int>(links_.size())) {
			if(const help::topic* t = find(links_[link_].second)) {
				open_topic(*t, true);
			} else {
				speak(_("This link leads to a topic you have not discovered yet."), msg::reply);
				cue("error");
			}
		} else {
			speak(accessibility::join(paragraphs_, " "), msg::reply);
		}
		return true;
	case SDLK_BACKSPACE:
		if(!history_.empty()) {
			const help::topic* t = history_.back();
			history_.pop_back();
			topic_ = nullptr;
			open_topic(*t, false);
		} else {
			reading_ = false;
			say_item(true);
		}
		return true;
	case SDLK_LEFT:
		reading_ = false;
		history_.clear();
		// Put the list position on the topic if it is visible in the list.
		for(std::size_t i = 0; i < items_.size(); ++i) {
			if(items_[i].top == topic_) {
				pos_ = i;
			}
		}
		say_item(true);
		return true;
	default:
		return false;
	}
}

void help_reader::handle_event(const SDL_Event& event)
{
	if(!enabled()) {
		return;
	}
	if(event.type == SDL_TEXTINPUT) {
		suppress_text_ = false;
		return;
	}
	if(event.type != SDL_KEYDOWN) {
		return;
	}
	if(handle_global_key(event)) {
		return;
	}
	const int key = event.key.keysym.sym;
	const unsigned mod = event.key.keysym.mod;
	if(key == SDLK_ESCAPE) {
		closed_ = true;
		return;
	}
	if(reading_) {
		handle_reading_key(key, mod);
	} else {
		handle_list_key(key, mod);
	}
}

} // namespace accessibility
