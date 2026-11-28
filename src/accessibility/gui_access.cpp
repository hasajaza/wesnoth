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

#include "accessibility/gui_access.hpp"

#include "accessibility/accessibility.hpp"
#include "accessibility/help_access.hpp"
#include "accessibility/keymap.hpp"

#include "actions/attack.hpp"
#include "attack_prediction.hpp"
#include "gettext.hpp"
#include "gui/auxiliary/find_widget.hpp"
#include "gui/dialogs/attack_predictions.hpp"
#include "gui/dialogs/unit_attack.hpp"
#include "gui/widgets/button.hpp"
#include "gui/widgets/container_base.hpp"
#include "gui/widgets/generator.hpp"
#include "gui/widgets/grid.hpp"
#include "gui/widgets/image.hpp"
#include "gui/widgets/label.hpp"
#include "gui/widgets/listbox.hpp"
#include "gui/widgets/menu_button.hpp"
#include "gui/widgets/multi_page.hpp"
#include "gui/widgets/multimenu_button.hpp"
#include "gui/widgets/password_box.hpp"
#include "gui/widgets/progress_bar.hpp"
#include "gui/widgets/repeating_button.hpp"
#include "gui/widgets/scroll_label.hpp"
#include "gui/widgets/scroll_text.hpp"
#include "gui/widgets/scrollbar_container.hpp"
#include "gui/widgets/slider.hpp"
#include "gui/widgets/spinner.hpp"
#include "gui/widgets/stacked_widget.hpp"
#include "gui/widgets/text_box.hpp"
#include "gui/widgets/toggle_button.hpp"
#include "gui/widgets/toggle_panel.hpp"
#include "gui/widgets/tree_view.hpp"
#include "gui/widgets/tree_view_node.hpp"
#include "gui/widgets/unit_preview_pane.hpp"
#include "gui/widgets/window.hpp"
#include "serialization/unicode.hpp"
#include "units/unit.hpp"

#include <SDL2/SDL_events.h>
#include <SDL2/SDL_timer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <set>
#include <vector>

namespace accessibility::gui
{
using namespace gui2;

namespace
{
enum class role
{
	button, check, toggle_panel, menu, multimenu, slider, text, password, spin, list, tree, long_text, sort_header
};

struct focusable
{
	widget* w;
	role r;
	std::string label_hint; /**< Preceding label text, used as name for unnamed controls. */
};

struct text_item
{
	widget* w;
	std::string id;
	std::string text;
	bool content; /**< Primary content of the window (message, story text...). */
};

struct collection
{
	std::vector<focusable> controls;
	std::vector<text_item> texts;
};

struct window_state
{
	widget* focus = nullptr;
	std::string focus_name;
	std::map<widget*, std::string> values;    /**< State snapshot of every control before a key. */
	std::map<widget*, std::string> content;   /**< Snapshot of content texts, for change detection. */
	uint32_t last_key_ticks = 0;
	uint32_t last_poll_ticks = 0;
	bool announced = false;
	bool suppress_text_input = false;
	std::size_t text_line = 0;  /**< Line reviewed in a focused long text. */
	std::vector<std::string> review_lines; /**< Lines read with Up/Down on buttons (damage calculation). */
	int review_pos = -1;
};

std::map<window*, window_state> g_windows;
std::vector<window*> g_stack;

const std::set<std::string> content_ids {
	"message", "part_text", "description", "objectives", "summary", "help_text",
	"campaign_description", "msg", "details",
};

std::string utf8_substr(const std::string& s, std::size_t pos, std::size_t len)
{
	std::size_t b = utf8::index(s, pos);
	std::size_t e = utf8::index(s, pos + len);
	if(b >= s.size()) {
		return std::string();
	}
	return s.substr(b, e - b);
}

bool is_shown(const widget& w)
{
	return w.get_visible() == widget::visibility::visible;
}

std::string clean(const std::string& s)
{
	return plain_text(s);
}

/** Converts an id like "button_new_game" or "txtFilter" into "new game" / "filter". */
std::string humanize(std::string id)
{
	for(const char* prefix : {"button_", "btn_", "_", "lbl_", "label_", "txt_", "txt"}) {
		std::string p(prefix);
		if(id.size() > p.size() && id.compare(0, p.size(), p) == 0) {
			id = id.substr(p.size());
		}
	}
	// camelCase -> separate lower-case words
	std::string res;
	for(std::size_t i = 0; i < id.size(); ++i) {
		const char c = id[i];
		if(std::isupper(static_cast<unsigned char>(c))) {
			if(!res.empty() && res.back() != ' ') {
				res += ' ';
			}
			res += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		} else {
			res += c;
		}
	}
	std::replace(res.begin(), res.end(), '_', ' ');
	std::replace(res.begin(), res.end(), '-', ' ');
	return res;
}

std::string label_of(const styled_widget& w)
{
	return clean(w.get_label().str());
}

std::string tooltip_of(const styled_widget& w)
{
	std::string t = clean(w.tooltip().str());
	if(t.empty()) {
		t = clean(w.help_message().str());
	}
	return t;
}

/** Texts of a row or node, in reading order. */
void collect_row_texts(widget& w, std::vector<std::string>& out);

void collect_grid_texts(grid& g, std::vector<std::string>& out)
{
	for(unsigned row = 0; row < g.get_rows(); ++row) {
		for(unsigned col = 0; col < g.get_cols(); ++col) {
			if(widget* c = g.get_widget(row, col)) {
				collect_row_texts(*c, out);
			}
		}
	}
}

void collect_row_texts(widget& w, std::vector<std::string>& out)
{
	if(!is_shown(w)) {
		return;
	}
	if(auto* g = dynamic_cast<grid*>(&w)) {
		collect_grid_texts(*g, out);
		return;
	}
	if(auto* im = dynamic_cast<image*>(&w)) {
		std::string t = tooltip_of(*im);
		if(!t.empty()) {
			out.push_back(t);
		}
		return;
	}
	if(auto* tb = dynamic_cast<toggle_button*>(&w)) {
		std::string l = label_of(*tb);
		if(l.empty()) {
			l = tooltip_of(*tb);
		}
		out.push_back(l + (l.empty() ? "" : " ") + (tb->get_value_bool() ? _("checked") : _("not checked")));
		return;
	}
	if(auto* sl = dynamic_cast<scroll_label*>(&w)) {
		out.push_back(label_of(*sl));
		return;
	}
	if(auto* c = dynamic_cast<container_base*>(&w)) {
		collect_grid_texts(c->get_grid(), out);
		return;
	}
	if(auto* sw = dynamic_cast<styled_widget*>(&w)) {
		if(dynamic_cast<label*>(sw) || dynamic_cast<button*>(sw)) {
			std::string l = label_of(*sw);
			if(l.empty() && dynamic_cast<button*>(sw)) {
				l = tooltip_of(*sw);
			}
			out.push_back(l);
		}
	}
}

std::string row_text(grid& g)
{
	std::vector<std::string> parts;
	collect_grid_texts(g, parts);
	// Remove consecutive duplicates (labels repeated in tooltips).
	parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
	return join(parts);
}

/*
 * Combat dialogs. The game's attack dialog shows each weapon as three
 * columns of numbers ("sword blade 6×3 80%, — melee —, flail impact 3×5
 * magical 70%") and the damage calculation as a graph. They are read as
 * sentences instead, from the same battle data the dialogs display. The
 * outcome probabilities are the game's own (already calculated for the
 * dialogs); nothing here uses the game's random number generator.
 */

std::string unit_label(const unit& u)
{
	return u.name().empty() ? u.type_name().str() : u.name().str();
}

std::string strikes_text(int n)
{
	return std::to_string(n) + " " + (n == 1 ? _("strike") : _("strikes"));
}

/** Probability (0..1) as spoken percentage: "0%", "0.2%", "35%". */
std::string percent_text(double p)
{
	const double v = p * 100.0;
	if(v <= 0.0) {
		return "0%";
	}
	if(v < 0.1) {
		return _("under 0.1%");
	}
	if(v >= 100.0) {
		return "100%";
	}
	if(v > 99.9) {
		return _("over 99.9%");
	}
	std::ostringstream o;
	if(v < 10.0 || v > 99.0) {
		o << std::fixed << std::setprecision(1) << v;
	} else {
		o << std::lround(v);
	}
	std::string r = o.str();
	if(r.size() > 2 && r.compare(r.size() - 2, 2, ".0") == 0) {
		r.erase(r.size() - 2);
	}
	return r + "%";
}

/** Lines of a label as plain text ("<b>sword</b>\nblade\n6×3 magical\n80%"). */
std::vector<std::string> label_lines(const std::string& markup)
{
	std::vector<std::string> lines;
	std::size_t start = 0;
	while(start <= markup.size()) {
		std::size_t nl = markup.find('\n', start);
		std::string line = plain_text(markup.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
		lines.push_back(line);
		if(nl == std::string::npos) {
			break;
		}
		start = nl + 1;
	}
	return lines;
}

/**
 * One side of a weapon row: "sword, blade: 6 damage, 3 strikes, 80% to hit,
 * magical". Words (name, type, specials) come from the dialog's own labels,
 * numbers from the battle statistics.
 */
std::string weapon_words(const std::string& label_markup, const battle_context_unit_stats& st, const std::string& range)
{
	std::vector<std::string> l = label_lines(label_markup);
	std::string name = l.size() > 0 ? l[0] : std::string();
	std::string type = l.size() > 1 ? l[1] : std::string();
	std::string specials;
	if(l.size() > 2) {
		// "6×3 magical, marksman": everything after the numbers.
		std::size_t sp = l[2].find(' ');
		if(sp != std::string::npos) {
			specials = l[2].substr(sp + 1);
		}
	}
	std::string head = join({name, range, type});
	// "3 strikes of 9 damage, 60% chance to hit each"
	std::vector<std::string> parts {
		strikes_text(st.num_blows) + " " + _("of") + " " + std::to_string(st.damage) + " " + _("damage"),
		std::to_string(st.chance_to_hit) + "% " + (st.num_blows == 1 ? _("chance to hit") : _("chance to hit each")),
		specials.empty() ? std::string() : _("specials") + std::string(" ") + specials,
	};
	return head + (head.empty() ? "" : ": ") + join(parts);
}

/** Outcome of a fight, from the game's own probability calculation. */
std::string outcome_text(battle_context& bc, const unit& au, const unit& du)
{
	const battle_context_unit_stats& as = bc.get_attacker_stats();
	const battle_context_unit_stats& ds = bc.get_defender_stats();
	const combatant& ca = bc.get_attacker_combatant();
	const combatant& cd = bc.get_defender_combatant();
	const std::string an = unit_label(au);
	const std::string dn = unit_label(du);
	std::vector<std::string> parts;
	parts.push_back(_("Chance to kill") + std::string(" ") + dn + ": " + percent_text(cd.hp_dist.empty() ? 0.0 : cd.hp_dist[0]));
	parts.push_back(_("Chance that") + std::string(" ") + an + " " + _("dies") + ": "
		+ percent_text(ca.hp_dist.empty() ? 0.0 : ca.hp_dist[0]));
	parts.push_back(_("Expected hit points after the fight") + std::string(": ") + an + " "
		+ std::to_string(std::lround(ca.average_hp())) + " " + _("of") + " " + std::to_string(as.max_hp) + ", " + dn + " "
		+ std::to_string(std::lround(cd.average_hp())) + " " + _("of") + " " + std::to_string(ds.max_hp));
	if(as.poisons && !ds.is_poisoned && cd.poisoned > 0.0) {
		parts.push_back(dn + " " + _("poisoned") + ": " + percent_text(cd.poisoned));
	}
	if(ds.poisons && !as.is_poisoned && ca.poisoned > 0.0) {
		parts.push_back(an + " " + _("poisoned") + ": " + percent_text(ca.poisoned));
	}
	if(as.slows && !ds.is_slowed && cd.slowed > 0.0) {
		parts.push_back(dn + " " + _("slowed") + ": " + percent_text(cd.slowed));
	}
	if(ds.slows && !as.is_slowed && ca.slowed > 0.0) {
		parts.push_back(an + " " + _("slowed") + ": " + percent_text(ca.slowed));
	}
	return join(parts, ". ");
}

/** A weapon row of the attack dialog as a sentence; empty for other lists. */
std::string attack_row_text(listbox& l, unsigned idx)
{
	auto* dlg = dynamic_cast<dialogs::unit_attack*>(l.get_window());
	if(!dlg || l.id() != "weapon_list") {
		return std::string();
	}
	std::vector<battle_context>& weapons = dlg->access_weapons();
	unit_const_ptr au = dlg->access_attacker();
	unit_const_ptr du = dlg->access_defender();
	grid* g = l.get_row_grid(idx);
	if(idx >= weapons.size() || !au || !du || !g) {
		return std::string();
	}
	battle_context& bc = weapons[idx];
	auto label_markup = [&](const std::string& id) {
		auto* w = find_widget<styled_widget>(g, id, false, false);
		return w ? w->get_label().str() : std::string();
	};
	std::string range = plain_text(label_markup("range"));
	// "— melee —"
	const std::string dash = "\xE2\x80\x94";
	std::size_t p;
	while((p = range.find(dash)) != std::string::npos) {
		range.erase(p, dash.size());
	}
	range = plain_text(range);

	std::vector<std::string> parts;
	parts.push_back(weapon_words(label_markup("attacker_weapon"), bc.get_attacker_stats(), range));
	if(bc.get_defender_stats().weapon && bc.get_defender_stats().num_blows > 0) {
		parts.push_back(unit_label(*du) + " " + _("strikes back with") + " "
			+ weapon_words(label_markup("defender_weapon"), bc.get_defender_stats(), std::string()));
	} else {
		parts.push_back(unit_label(*du) + " " + _("cannot strike back"));
	}
	parts.push_back(outcome_text(bc, *au, *du));
	return join(parts, ". ");
}

/** Short introduction of the attack dialog: who attacks whom. */
std::string attack_intro(window& win)
{
	auto* dlg = dynamic_cast<dialogs::unit_attack*>(&win);
	if(!dlg) {
		return std::string();
	}
	unit_const_ptr au = dlg->access_attacker();
	unit_const_ptr du = dlg->access_defender();
	if(!au || !du) {
		return std::string();
	}
	auto who = [](const unit& u) {
		std::string s = unit_label(u);
		if(!u.name().empty()) {
			s += ", " + u.type_name().str();
		}
		return s + ", " + _("level") + " " + std::to_string(u.level()) + ", " + std::to_string(u.hitpoints()) + " "
			+ _("of") + " " + std::to_string(u.max_hitpoints()) + " " + _("hit points");
	};
	return who(*au) + " " + _("attacks") + " " + who(*du) + ". "
		+ _("Up and Down choose the weapon; for each weapon you hear your damage, the enemy's answer and the chances. Enter attacks, Escape cancels, Tab reaches Damage Calculation and the unit profiles.");
}

/** The damage calculation dialog as lines: outcome first, then each side's details. */
std::vector<std::string> prediction_lines(window& win)
{
	std::vector<std::string> lines;
	auto* dlg = dynamic_cast<dialogs::attack_predictions*>(&win);
	if(!dlg) {
		return lines;
	}
	unit_const_ptr au = dlg->access_unit(true);
	unit_const_ptr du = dlg->access_unit(false);
	const std::string an = au ? unit_label(*au) : std::string(_("Attacker"));
	const std::string dn = du ? unit_label(*du) : std::string(_("Defender"));
	const combatant& ca = dlg->access_combatant(true);
	const combatant& cd = dlg->access_combatant(false);

	lines.push_back(_("Chance to kill") + std::string(" ") + dn + ": " + percent_text(cd.hp_dist.empty() ? 0.0 : cd.hp_dist[0]) + ". "
		+ _("Chance that") + " " + an + " " + _("dies") + ": " + percent_text(ca.hp_dist.empty() ? 0.0 : ca.hp_dist[0]));

	static const char* const ids[] {
		"base_damage", "tod_modifier", "leadership_modifier", "slowed_modifier", "resis", "total_damage", "chance_to_hit", "chance_unscathed",
	};
	for(bool att : {true, false}) {
		const std::string who = att ? an : dn;
		const std::string prefix = att ? "attacker_" : "defender_";
		std::vector<std::string> parts;
		for(const char* id : ids) {
			label* v = find_widget<label>(&win, prefix + id, false, false);
			if(!v || !is_shown(*v)) {
				continue;
			}
			std::string value = label_of(*v);
			if(value.empty()) {
				continue;
			}
			std::string name;
			if(label* n = find_widget<label>(&win, prefix + id + "_label", false, false)) {
				name = label_of(*n);
				while(!name.empty() && (name.back() == ':' || name.back() == ' ')) {
					name.pop_back();
				}
			}
			parts.push_back(name.empty() ? value : name + " " + value);
		}
		lines.push_back((att ? _("Attacker") : _("Defender")) + std::string(" ") + who + ": " + join(parts));

		const combatant& cb = att ? ca : cd;
		std::vector<std::string> hp;
		for(int h = static_cast<int>(cb.hp_dist.size()) - 1; h >= 0; --h) {
			if(cb.hp_dist[h] >= 0.0005) {
				hp.push_back(std::to_string(h) + " " + _("hit points") + " " + percent_text(cb.hp_dist[h]));
			}
		}
		const auto& st = dlg->access_stats(att);
		lines.push_back(who + " " + _("after the fight") + ": " + join(hp) + ". " + _("Expected") + " "
			+ std::to_string(std::lround(cb.average_hp())) + " " + _("of") + " " + std::to_string(st.max_hp) + " " + _("hit points"));
	}
	return lines;
}

/** Replaces the generic text of some windows; empty when the generic reading is used. */
std::string special_content(window& win, std::vector<std::string>* review)
{
	if(win.id() == "unit_attack") {
		return attack_intro(win);
	}
	if(win.id() == "attack_predictions") {
		std::vector<std::string> lines = prediction_lines(win);
		if(lines.empty()) {
			return std::string();
		}
		if(review) {
			*review = lines;
		}
		return join(lines, ". ") + ". " + _("Up and Down read these lines one at a time.");
	}
	return std::string();
}

/** Descends through single-cell wrappers (toggle panels, panels, grids) to the grid holding the columns. */
grid* columns_grid(grid& g)
{
	grid* cur = &g;
	for(int depth = 0; depth < 6; ++depth) {
		if(cur->get_rows() != 1 || cur->get_cols() != 1) {
			break;
		}
		widget* w = cur->get_widget(0, 0);
		if(auto* cb = dynamic_cast<container_base*>(w)) {
			cur = &cb->get_grid();
		} else if(auto* gg = dynamic_cast<grid*>(w)) {
			cur = gg;
		} else {
			break;
		}
	}
	return cur;
}

/** Caption of a list header cell: the labels of its labels and sort buttons, without states. */
void header_texts(widget& w, std::vector<std::string>& out)
{
	if(auto* g = dynamic_cast<grid*>(&w)) {
		for(unsigned row = 0; row < g->get_rows(); ++row) {
			for(unsigned col = 0; col < g->get_cols(); ++col) {
				if(widget* c = g->get_widget(row, col)) {
					header_texts(*c, out);
				}
			}
		}
		return;
	}
	if(auto* cb = dynamic_cast<container_base*>(&w)) {
		header_texts(cb->get_grid(), out);
		return;
	}
	if(dynamic_cast<label*>(&w) || dynamic_cast<toggle_button*>(&w) || dynamic_cast<button*>(&w)) {
		std::string t = label_of(*static_cast<styled_widget*>(&w));
		if(t.empty()) {
			t = tooltip_of(*static_cast<styled_widget*>(&w));
		}
		out.push_back(t);
	}
}

/**
 * A row of a list with column headers (unit list, status table, statistics)
 * read as "Type Spearman, Name Pike, HP 36/36": a sighted player reads every
 * value under its column title. Empty when the list has no matching header.
 */
std::string row_with_headers(listbox& l, grid& row)
{
	grid* header = find_widget<grid>(&l, "_header_grid", false, false);
	if(!header || !is_shown(*header)) {
		return std::string();
	}
	grid* hc = columns_grid(*header);
	grid* rc = columns_grid(row);
	if(hc->get_rows() != 1 || rc->get_rows() != 1 || hc->get_cols() != rc->get_cols() || hc->get_cols() < 2) {
		return std::string();
	}
	std::vector<std::string> parts;
	for(unsigned col = 0; col < rc->get_cols(); ++col) {
		widget* cell = rc->get_widget(0, col);
		if(!cell) {
			continue;
		}
		std::vector<std::string> v;
		collect_row_texts(*cell, v);
		std::string value = join(v);
		if(value.empty()) {
			continue;
		}
		std::vector<std::string> h;
		if(widget* hcell = hc->get_widget(0, col)) {
			header_texts(*hcell, h);
		}
		std::string head = join(h, " ");
		parts.push_back(head.empty() || head == value ? value : head + " " + value);
	}
	return join(parts);
}

/** Text of one list row, with the special cases above. */
std::string list_row_text(listbox& l, unsigned idx)
{
	std::string special = attack_row_text(l, idx);
	if(!special.empty()) {
		return special;
	}
	grid* g = l.get_row_grid(idx);
	if(!g) {
		return std::string();
	}
	std::string with_headers = row_with_headers(l, *g);
	return with_headers.empty() ? row_text(*g) : with_headers;
}

/** The unit a "Profile" button belongs to ("Profile of Konrad"), if any. */
std::string profile_owner(widget& w)
{
	for(widget* p = w.parent(); p; p = p->parent()) {
		if(dynamic_cast<unit_preview_pane*>(p)) {
			if(label* n = find_widget<label>(p, "type_name", false, false)) {
				if(is_shown(*n) && !label_of(*n).empty()) {
					return label_of(*n);
				}
			}
			// Compact panes (attack dialog): "name\ntype\nLvl 1..." in one label.
			if(auto* d = find_widget<styled_widget>(p, "type_details_minimal", false, false)) {
				std::vector<std::string> l = label_lines(d->get_label().str());
				std::vector<std::string> who;
				for(std::size_t i = 0; i < l.size() && i < 2; ++i) {
					if(!l[i].empty() && (who.empty() || who.back() != l[i])) {
						who.push_back(l[i]);
					}
				}
				return join(who);
			}
			return std::string();
		}
	}
	return std::string();
}

generator_base* list_generator(listbox& l)
{
	return find_widget<generator_base>(&l, "_list_grid", false, false);
}

/** Selected row position among the shown rows, in display order, and the shown count. */
void list_position(listbox& l, int& pos, int& count)
{
	pos = -1;
	count = 0;
	const int sel = l.get_selected_row();
	generator_base* gen = list_generator(l);
	const unsigned n = l.get_item_count();
	for(unsigned ordered = 0; ordered < n; ++ordered) {
		unsigned idx = gen ? gen->get_item_at_ordered(ordered) : ordered;
		if(idx >= n) {
			continue;
		}
		if(gen && !gen->get_item_shown(idx)) {
			continue;
		}
		if(static_cast<int>(idx) == sel) {
			pos = count;
		}
		++count;
	}
}

std::string node_text(tree_view_node& n)
{
	return row_text(n.get_grid());
}

void collect(widget& w, collection& c, std::string& pending_label);

void collect_grid(grid& g, collection& c, std::string& pending_label)
{
	for(unsigned row = 0; row < g.get_rows(); ++row) {
		for(unsigned col = 0; col < g.get_cols(); ++col) {
			if(widget* child = g.get_widget(row, col)) {
				collect(*child, c, pending_label);
			}
		}
	}
}

void add_control(collection& c, widget& w, role r, std::string& pending_label)
{
	// Three-state toggles named sort_* (campaign list "Name" and "Timeline")
	// are sort buttons, not check boxes.
	if(r == role::check && w.id().compare(0, 5, "sort_") == 0) {
		if(auto* t = dynamic_cast<toggle_button*>(&w); t && t->num_states() == 3) {
			r = role::sort_header;
		}
	}
	c.controls.push_back({&w, r, pending_label});
	pending_label.clear();
}

void collect(widget& w, collection& c, std::string& pending_label)
{
	if(!is_shown(w)) {
		return;
	}

	if(auto* g = dynamic_cast<grid*>(&w)) {
		collect_grid(*g, c, pending_label);
		return;
	}
	if(auto* l = dynamic_cast<listbox*>(&w)) {
		if(grid* header = find_widget<grid>(l, "_header_grid", false, false)) {
			std::string none;
			collection hc;
			collect_grid(*header, hc, none);
			for(auto& f : hc.controls) {
				if(f.r == role::check) {
					f.r = role::sort_header;
				}
				c.controls.push_back(f);
			}
		}
		add_control(c, *l, role::list, pending_label);
		return;
	}
	if(auto* t = dynamic_cast<tree_view*>(&w)) {
		add_control(c, *t, role::tree, pending_label);
		return;
	}
	if(auto* sl = dynamic_cast<scroll_label*>(&w)) {
		std::string text = label_of(*sl);
		if(!text.empty()) {
			bool content = content_ids.count(sl->id()) > 0;
			c.texts.push_back({sl, sl->id(), text, content});
			// Long texts are reachable with Tab so they can be read line by line.
			if(text.size() > 120 || content) {
				add_control(c, *sl, role::long_text, pending_label);
			}
		}
		return;
	}
	if(auto* st = dynamic_cast<scroll_text*>(&w)) {
		std::string text = label_of(*st);
		if(!text.empty()) {
			c.texts.push_back({st, st->id(), text, content_ids.count(st->id()) > 0});
			add_control(c, *st, role::long_text, pending_label);
		}
		return;
	}
	if(auto* mp = dynamic_cast<multi_page*>(&w)) {
		int page = mp->get_selected_page();
		if(page >= 0 && static_cast<unsigned>(page) < mp->get_page_count()) {
			collect_grid(mp->page_grid(page), c, pending_label);
		}
		return;
	}
	if(auto* sw = dynamic_cast<stacked_widget*>(&w)) {
		for(unsigned i = 0; i < sw->get_layer_count(); ++i) {
			if(grid* layer = sw->get_layer_grid(i)) {
				if(is_shown(*layer)) {
					collect_grid(*layer, c, pending_label);
				}
			}
		}
		return;
	}
	if(auto* sp = dynamic_cast<spinner*>(&w)) {
		add_control(c, *sp, role::spin, pending_label);
		return;
	}
	if(auto* tp = dynamic_cast<toggle_panel*>(&w)) {
		add_control(c, *tp, role::toggle_panel, pending_label);
		return;
	}
	if(auto* sc = dynamic_cast<scrollbar_container*>(&w)) {
		if(grid* g = sc->content_grid()) {
			collect_grid(*g, c, pending_label);
		}
		return;
	}
	if(auto* cb = dynamic_cast<container_base*>(&w)) {
		collect_grid(cb->get_grid(), c, pending_label);
		return;
	}

	if(dynamic_cast<password_box*>(&w)) {
		add_control(c, w, role::password, pending_label);
		return;
	}
	if(dynamic_cast<text_box*>(&w)) {
		add_control(c, w, role::text, pending_label);
		return;
	}
	if(dynamic_cast<button*>(&w) || dynamic_cast<repeating_button*>(&w)) {
		add_control(c, w, role::button, pending_label);
		return;
	}
	if(dynamic_cast<toggle_button*>(&w)) {
		add_control(c, w, role::check, pending_label);
		return;
	}
	if(dynamic_cast<menu_button*>(&w)) {
		add_control(c, w, role::menu, pending_label);
		return;
	}
	if(dynamic_cast<multimenu_button*>(&w)) {
		add_control(c, w, role::multimenu, pending_label);
		return;
	}
	if(dynamic_cast<slider*>(&w)) {
		add_control(c, w, role::slider, pending_label);
		return;
	}
	if(auto* pb = dynamic_cast<progress_bar*>(&w)) {
		c.texts.push_back({pb, pb->id(), _("progress") + std::string(" ") + std::to_string(pb->get_percentage()) + "%", false});
		return;
	}
	if(auto* im = dynamic_cast<image*>(&w)) {
		std::string t = tooltip_of(*im);
		if(!t.empty()) {
			c.texts.push_back({im, im->id(), t, false});
		}
		return;
	}
	if(auto* lb = dynamic_cast<label*>(&w)) {
		std::string text = label_of(*lb);
		if(!text.empty()) {
			c.texts.push_back({lb, lb->id(), text, content_ids.count(lb->id()) > 0});
			// Short captions name the next control; pure values ("2", "50%") do not.
			const bool is_value = text.find_first_not_of("0123456789.,%+-/ \xE2\x88\x92") == std::string::npos;
			if(text.size() < 80 && !is_value) {
				pending_label = text;
			}
		}
		return;
	}
}

collection collect_window(window& win)
{
	collection c;
	std::string pending;
	collect(win.get_grid(), c, pending);
	if(win.id() == "outro") {
		// The outro draws its text directly on the window canvas.
		try {
			wfl::variant v = win.get_canvas(0).access_variable("outro_text");
			std::string text = v.is_string() ? plain_text(v.as_string()) : std::string();
			if(!text.empty()) {
				c.texts.push_back({&win, "outro_text", text, true});
			}
		} catch(...) {
		}
	}
	return c;
}

int index_of(const collection& c, const widget* w)
{
	for(std::size_t i = 0; i < c.controls.size(); ++i) {
		if(c.controls[i].w == w) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

bool control_active(const focusable& f)
{
	if(auto* sw = dynamic_cast<styled_widget*>(f.w)) {
		return sw->get_active();
	}
	if(auto* sp = dynamic_cast<spinner*>(f.w)) {
		return sp->get_active();
	}
	return true;
}

std::string role_name(role r)
{
	switch(r) {
	case role::button: return _("button");
	case role::check: return _("check box");
	case role::toggle_panel: return _("toggle");
	case role::menu: return _("menu button");
	case role::multimenu: return _("multi select menu");
	case role::slider: return _("slider");
	case role::text: return _("edit");
	case role::password: return _("password edit");
	case role::spin: return _("spin box");
	case role::list: return _("list");
	case role::tree: return _("tree view");
	case role::long_text: return _("text");
	case role::sort_header: return _("sort button");
	}
	return std::string();
}

std::string control_name(const focusable& f)
{
	std::string name;
	const bool labelled = f.r == role::button || f.r == role::check || f.r == role::sort_header || f.r == role::multimenu;
	if(auto* sw = dynamic_cast<styled_widget*>(f.w)) {
		if(labelled) {
			name = label_of(*sw);
		}
	}
	if(f.r == role::multimenu && !f.label_hint.empty()) {
		// Its caption shows the selection ("None"); the label before it names it ("Modifications").
		name.clear();
	}
	if(f.r == role::toggle_panel) {
		name = row_text(static_cast<toggle_panel*>(f.w)->get_grid());
	}
	if(f.r == role::button && f.w->id() == "type_profile") {
		// Two "Profile" buttons in the attack dialog: say whose profile.
		std::string owner = profile_owner(*f.w);
		if(!owner.empty()) {
			name = _("Profile of") + std::string(" ") + owner;
		}
	}
	// Controls without their own caption (sliders, fields, menus, lists) are
	// named by the visible label before them, like a sighted user reads them.
	if(name.empty()) {
		name = f.label_hint;
		while(!name.empty() && (name.back() == ':' || name.back() == ' ')) {
			name.pop_back();
		}
	}
	if(name.empty()) {
		if(auto* sw = dynamic_cast<styled_widget*>(f.w)) {
			name = tooltip_of(*sw);
		}
	}
	if(name.empty() && f.r != role::long_text && f.r != role::list) {
		name = humanize(f.w->id());
	}
	return name;
}

/** Value/state part of a control description; also used for change detection. */
std::string control_value(const focusable& f, bool for_snapshot)
{
	switch(f.r) {
	case role::check:
	case role::sort_header: {
		auto* t = static_cast<toggle_button*>(f.w);
		if(f.r == role::sort_header) {
			unsigned v = t->get_value();
			return v == 0 ? _("not sorted") : (v == 1 ? _("sorted ascending") : _("sorted descending"));
		}
		if(t->num_states() > 2) {
			return _("state") + std::string(" ") + std::to_string(t->get_value() + 1) + " of " + std::to_string(t->num_states());
		}
		return t->get_value_bool() ? _("checked") : _("not checked");
	}
	case role::toggle_panel:
		return static_cast<toggle_panel*>(f.w)->get_value_bool() ? _("selected") : _("not selected");
	case role::menu: {
		auto* m = static_cast<menu_button*>(f.w);
		std::string v = clean(m->get_label().str());
		if(v.empty()) {
			v = clean(m->get_value_string());
		}
		return v + (for_snapshot ? "" : ", " + std::to_string(m->get_value() + 1) + " of " + std::to_string(m->num_states()));
	}
	case role::multimenu: {
		auto* m = static_cast<multimenu_button*>(f.w);
		std::string v = f.label_hint.empty() ? std::string() : clean(m->get_label().str());
		return join({v, std::to_string(m->get_toggle_states().count()) + " " + _("selected")});
	}
	case role::slider: {
		auto* s = static_cast<slider*>(f.w);
		std::string v = clean(s->get_value_label().str());
		if(v.empty()) {
			v = std::to_string(s->get_value());
		}
		if(for_snapshot) {
			return v;
		}
		return v + ", " + _("minimum") + " " + std::to_string(s->get_minimum_value()) + ", " + _("maximum") + " "
			+ std::to_string(s->get_maximum_value());
	}
	case role::text: {
		auto* t = static_cast<text_box*>(f.w);
		std::string v = t->get_value();
		if(for_snapshot) {
			return v + "\x01" + std::to_string(t->access_selection_start()) + "\x01" + std::to_string(t->access_selection_length());
		}
		return v.empty() ? _("blank") : v;
	}
	case role::password: {
		auto* t = static_cast<text_box*>(f.w);
		std::size_t n = utf8::size(t->get_value());
		return for_snapshot ? std::to_string(n) + "\x01" + std::to_string(t->access_selection_start())
							: (n == 0 ? std::string(_("blank")) : std::to_string(n) + " " + _("characters"));
	}
	case role::spin: {
		auto* s = static_cast<spinner*>(f.w);
		return std::to_string(s->get_value());
	}
	case role::list: {
		auto* l = static_cast<listbox*>(f.w);
		int sel = l->get_selected_row();
		int pos, count;
		list_position(*l, pos, count);
		if(count == 0) {
			return _("empty");
		}
		if(sel < 0 || pos < 0) {
			return _("no item selected") + std::string(", ") + std::to_string(count) + " " + _("items");
		}
		std::string text = list_row_text(*l, sel);
		if(for_snapshot) {
			return std::to_string(sel) + "\x01" + text;
		}
		return text + ", " + std::to_string(pos + 1) + " of " + std::to_string(count);
	}
	case role::tree: {
		auto* t = static_cast<tree_view*>(f.w);
		tree_view_node* n = t->selected_item();
		if(!n) {
			return _("no item selected");
		}
		std::string text = node_text(*n);
		std::string state;
		if(n->count_children() > 0) {
			state = n->is_folded() ? _("collapsed") : _("expanded");
		}
		if(for_snapshot) {
			return text + "\x01" + state + "\x01" + std::to_string(reinterpret_cast<std::uintptr_t>(n));
		}
		const unsigned depth = n->get_indentation_level();
		return join({text, state, depth > 1 ? _("depth") + std::string(" ") + std::to_string(depth) : std::string()});
	}
	case role::long_text:
		return for_snapshot ? label_of(*static_cast<styled_widget*>(f.w)) : std::string();
	case role::button:
		return std::string();
	}
	return std::string();
}

std::string describe(const focusable& f)
{
	std::string name = control_name(f);
	std::vector<std::string> parts;
	if(f.r == role::long_text) {
		parts.push_back(label_of(*static_cast<styled_widget*>(f.w)));
	} else {
		parts.push_back(name);
		parts.push_back(role_name(f.r));
		parts.push_back(control_value(f, false));
	}
	if(!control_active(f)) {
		parts.push_back(_("unavailable"));
	}
	return join(parts);
}

std::string window_title(window& win, const collection& c)
{
	if(label* t = find_widget<label>(&win, "title", false, false)) {
		if(is_shown(*t)) {
			std::string s = label_of(*t);
			if(!s.empty()) {
				return s;
			}
		}
	}
	if(win.id().compare(0, 11, "wml_message") == 0 || win.id() == "transient_message" || win.id() == "message") {
		// A message without a speaker (narrator): no title.
		return std::string();
	}
	static const std::map<std::string, const char*> known {
		{"outro", N_("End of scenario")},
		{"title_screen", N_("Main menu")},
		{"campaign_selection", N_("Campaign selection")},
		{"campaign_difficulty", N_("Select difficulty")},
		{"game_load", N_("Load game")},
		{"game_save", N_("Save game")},
		{"preferences", N_("Preferences")},
		{"unit_recruit", N_("Recruit")},
		{"unit_recall", N_("Recall")},
		{"unit_attack", N_("Attack")},
		{"unit_advance", N_("Advance unit")},
		{"story_viewer", N_("Story")},
		{"help_browser", N_("Help")},
		{"loading_screen", N_("Loading")},
		{"language_selection", N_("Language")},
		{"drop_down_menu", N_("Menu")},
		{"mp_method_selection", N_("Multiplayer")},
		{"mp_create_game", N_("Create game")},
		{"mp_staging", N_("Game setup")},
		{"addon_manager", N_("Add-ons")},
		{"end_credits", N_("Credits")},
	};
	auto it = known.find(win.id());
	if(it != known.end()) {
		return translation::gettext(it->second);
	}
	for(const text_item& t : c.texts) {
		if(t.text.size() < 60 && dynamic_cast<label*>(t.w)) {
			return t.text;
		}
	}
	return humanize(win.id());
}

window_state& state_of(window& w)
{
	return g_windows[&w];
}

void snapshot(window& win, const collection& c, window_state& st)
{
	st.values.clear();
	for(const focusable& f : c.controls) {
		st.values[f.w] = control_value(f, true) + (control_active(f) ? "" : "\x02");
	}
	st.content.clear();
	for(const text_item& t : c.texts) {
		if(t.content) {
			st.content[t.w] = t.text;
		}
	}
	(void)win;
}

/** Makes the GUI2 keyboard focus follow the accessibility focus. */
void sync_native_focus(window& win, const focusable& f)
{
	switch(f.r) {
	case role::list:
	case role::tree:
	case role::text:
	case role::password:
	case role::slider:
	case role::long_text:
		win.keyboard_capture(f.w);
		break;
	case role::spin:
		if(text_box* tb = find_widget<text_box>(f.w, "_text", false, false)) {
			win.keyboard_capture(tb);
		}
		break;
	default:
		// Buttons and toggles: give focus so typing does not go to a text field.
		win.keyboard_capture(f.w);
		break;
	}
}

void set_focus(window& win, window_state& st, const collection& c, int index, bool announce)
{
	if(index < 0 || index >= static_cast<int>(c.controls.size())) {
		return;
	}
	const focusable& f = c.controls[index];
	st.focus = f.w;
	st.focus_name = control_name(f);
	st.text_line = 0;
	sync_native_focus(win, f);
	if(announce) {
		speak(describe(f), msg::nav);
	}
	debug("gui focus: " + win.id() + " / " + f.w->id() + " (" + role_name(f.r) + ")");
}

/** Chooses the initial focus: the native keyboard focus if it is a control, else the first control. */
int initial_focus(window& win, const collection& c)
{
	(void)win;
	// Prefer a list, then a tree or text field (where the native focus usually
	// is), then an OK-like button.
	for(role wanted : {role::list, role::tree}) {
		for(std::size_t i = 0; i < c.controls.size(); ++i) {
			if(c.controls[i].r == wanted && control_active(c.controls[i])) {
				return static_cast<int>(i);
			}
		}
	}
	for(std::size_t i = 0; i < c.controls.size(); ++i) {
		role r = c.controls[i].r;
		if((r == role::text) && control_active(c.controls[i])) {
			return static_cast<int>(i);
		}
	}
	for(std::size_t i = 0; i < c.controls.size(); ++i) {
		if(c.controls[i].r != role::long_text && c.controls[i].r != role::sort_header && control_active(c.controls[i])) {
			return static_cast<int>(i);
		}
	}
	return c.controls.empty() ? -1 : 0;
}

bool is_message_window(const std::string& id)
{
	return id == "message" || id == "transient_message" || id.compare(0, 11, "wml_message") == 0 || id == "story_viewer"
		|| id == "outro" || id == "game_delete" || id == "prompt" || id == "folder_create" || id == "edit_text"
		|| id == "wml_error" || id == "achievements_dialog" || id == "end_credits";
}

std::string content_text(const collection& c)
{
	std::vector<std::string> parts;
	for(const text_item& t : c.texts) {
		if(t.content && t.id != "title") {
			parts.push_back(t.text);
		}
	}
	return join(parts, ". ");
}

std::string static_text(const collection& c, const std::string& title)
{
	std::vector<std::string> parts;
	for(const text_item& t : c.texts) {
		if(t.text != title) {
			parts.push_back(t.text);
		}
	}
	return join(parts, ". ");
}

void record_dialogue(window& win, const collection& c)
{
	const std::string& id = win.id();
	if(id.compare(0, 11, "wml_message") == 0 || id == "story_viewer") {
		std::string speaker;
		if(label* t = find_widget<label>(&win, "title", false, false)) {
			speaker = label_of(*t);
		}
		std::string text = content_text(c);
		add_dialogue(speaker, text);
	}
}

} // anonymous namespace

std::string describe_widget(widget& w)
{
	collection c;
	std::string pending;
	collect(w, c, pending);
	if(!c.controls.empty()) {
		return describe(c.controls.front());
	}
	if(!c.texts.empty()) {
		return c.texts.front().text;
	}
	return std::string();
}

std::string read_all_text(window& win)
{
	collection c = collect_window(win);
	std::string title = window_title(win, c);
	std::vector<std::string> parts {title};
	// Interleave texts and controls in reading order is not tracked; read texts first, then controls.
	std::string special = special_content(win, nullptr);
	parts.push_back(special.empty() ? static_text(c, title) : special);
	for(const focusable& f : c.controls) {
		if(f.r == role::long_text) {
			continue;
		}
		parts.push_back(describe(f));
		if(f.r == role::list) {
			// All choices, in display order (for example the advancement options).
			auto* l = static_cast<listbox*>(f.w);
			generator_base* gen = list_generator(*l);
			const unsigned n = l->get_item_count();
			std::vector<std::string> rows;
			for(unsigned ordered = 0; ordered < n && rows.size() < 60; ++ordered) {
				unsigned idx = gen ? gen->get_item_at_ordered(ordered) : ordered;
				if(idx >= n || (gen && !gen->get_item_shown(idx))) {
					continue;
				}
				rows.push_back(list_row_text(*l, idx));
			}
			if(!rows.empty()) {
				parts.push_back(_("items") + std::string(": ") + join(rows, "; "));
			}
		}
	}
	return join(parts, ". ");
}

bool dialog_active()
{
	return !g_stack.empty();
}

std::size_t stack_depth()
{
	return g_stack.size();
}

std::string top_window_id()
{
	return g_stack.empty() ? std::string() : g_stack.back()->id();
}

void window_opened(window& win)
{
	if(win.mode() != window::show_mode::modal) {
		return;
	}
	g_stack.push_back(&win);
	window_state& st = state_of(win);
	st = window_state();
	st.last_poll_ticks = SDL_GetTicks();
	log("dialog opened: " + win.id());
	if(!enabled()) {
		return;
	}

	if(win.id() == "loading_screen") {
		// Loading happens in several short stages; say it once.
		static uint32_t last_loading = 0;
		uint32_t now = SDL_GetTicks();
		if(last_loading == 0 || now - last_loading > 20000) {
			speak(_("Loading"), msg::reply);
		}
		last_loading = now;
		st.announced = true;
		return;
	}

	collection c = collect_window(win);
	std::string title = window_title(win, c);
	std::vector<std::string> parts {title};

	if(win.id() == "title_screen") {
		// The main menu: start on "Campaigns"; the tip of the day is read with F4.
		int idx = -1;
		for(std::size_t i = 0; i < c.controls.size(); ++i) {
			if(c.controls[i].w->id() == "campaign") {
				idx = static_cast<int>(i);
			}
		}
		if(idx < 0) {
			idx = initial_focus(win, c);
		}
		if(idx >= 0) {
			set_focus(win, st, c, idx, false);
			parts.push_back(describe(c.controls[idx]));
		}
		parts.push_back(_("Press F4 to read the whole screen, F2 for help."));
		cue("dialog");
		speak(join(parts, ". "), msg::reply);
		snapshot(win, c, st);
		st.announced = true;
		return;
	}

	if(is_message_window(win.id())) {
		std::string content = content_text(c);
		parts.push_back(content.empty() ? static_text(c, title) : content);
		record_dialogue(win, c);
	} else if(std::string special = special_content(win, &st.review_lines); !special.empty()) {
		parts.push_back(special);
	} else {
		std::string content = content_text(c);
		std::string all = static_text(c, title);
		if(all.size() <= 300) {
			parts.push_back(all);
		} else if(!content.empty() && content.size() <= 600) {
			parts.push_back(content);
		}
	}

	int idx = initial_focus(win, c);
	if(win.id() == "message" && (title == translation::gettext("Quit") || title == translation::gettext("Clear Labels"))) {
		// The game's quit and clear labels questions: start on "No" so that
		// an Enter pressed by habit does not end the game or delete labels
		// (Tab reaches "Yes").
		for(std::size_t i = 0; i < c.controls.size(); ++i) {
			if(c.controls[i].w->id() == "cancel") {
				idx = static_cast<int>(i);
			}
		}
	}
	if(idx >= 0) {
		const focusable& f = c.controls[idx];
		// Do not repeat a long text that was just read as content.
		if(!(f.r == role::long_text && is_message_window(win.id()))) {
			parts.push_back(describe(f));
		}
		set_focus(win, st, c, idx, false);
	}
	cue("dialog");
	speak(join(parts, ". "), msg::reply);
	snapshot(win, c, st);
	st.announced = true;
}

void window_closed(window& win)
{
	auto it = std::find(g_stack.begin(), g_stack.end(), &win);
	if(it == g_stack.end()) {
		return;
	}
	g_stack.erase(it);
	g_windows.erase(&win);
	log("dialog closed: " + win.id() + " retval " + std::to_string(win.get_retval()));
	if(!enabled()) {
		return;
	}
	// Re-announce the focus of the window below, if any.
	if(!g_stack.empty()) {
		window& below = *g_stack.back();
		window_state& st = state_of(below);
		collection c = collect_window(below);
		int idx = index_of(c, st.focus);
		if(idx < 0) {
			idx = initial_focus(below, c);
		}
		if(idx >= 0) {
			set_focus(below, st, c, idx, false);
			speak(describe(c.controls[idx]), msg::event);
		}
		snapshot(below, c, st);
	}
}

void focus_regained()
{
	if(!enabled()) {
		return;
	}
	// The help reader is above every dialog that was open when it started.
	if(help_reader_refocus(g_stack.size())) {
		return;
	}
	if(!g_stack.empty()) {
		window& win = *g_stack.back();
		collection c = collect_window(win);
		window_state& st = state_of(win);
		int idx = index_of(c, st.focus);
		std::string text = window_title(win, c);
		if(idx >= 0) {
			text += ". " + describe(c.controls[idx]);
		}
		speak(text, msg::reply);
	}
}

namespace
{
void announce_changes(window& win, window_state& st, bool from_key)
{
	collection c = collect_window(win);
	std::vector<std::string> said;

	// Focus may have disappeared (control hidden or rebuilt): move to a sensible control.
	int idx = index_of(c, st.focus);
	if(idx < 0 && !c.controls.empty()) {
		// Try to find a control with the same name (lists rebuilt with new widgets).
		for(std::size_t i = 0; i < c.controls.size(); ++i) {
			if(!st.focus_name.empty() && control_name(c.controls[i]) == st.focus_name) {
				idx = static_cast<int>(i);
				break;
			}
		}
		if(idx < 0) {
			idx = initial_focus(win, c);
		}
		if(idx >= 0) {
			set_focus(win, st, c, idx, false);
			said.push_back(describe(c.controls[idx]));
		}
	}

	// Changed control values.
	for(const focusable& f : c.controls) {
		auto old = st.values.find(f.w);
		if(old == st.values.end()) {
			continue;
		}
		std::string now = control_value(f, true) + (control_active(f) ? "" : "\x02");
		if(now == old->second) {
			continue;
		}
		if(f.w == st.focus || from_key) {
			if(f.r == role::text || f.r == role::password) {
				if(f.w != st.focus) {
					continue;
				}
				auto* tb = static_cast<text_box*>(f.w);
				// Speak the character at the cursor after cursor movement, or the deleted text.
				std::string before = old->second.substr(0, old->second.find('\x01'));
				std::string value = tb->get_value();
				if(f.r == role::password) {
					continue; // never speak password contents
				}
				if(value.size() < before.size()) {
					// Deletion: say what was removed when it is a single character or word.
					std::size_t i = 0;
					while(i < value.size() && value[i] == before[i]) {
						++i;
					}
					said.push_back(before.substr(i, before.size() - value.size()));
				} else if(value == before) {
					std::size_t pos = tb->access_selection_start();
					std::size_t len = tb->access_selection_length();
					if(len > 0) {
						said.push_back(_("selected") + std::string(" ") + utf8_substr(value, pos, len));
					} else if(pos >= utf8::size(value)) {
						said.push_back(_("end"));
					} else {
						std::string ch = utf8_substr(value, pos, 1);
						said.push_back(ch == " " ? std::string(_("space")) : ch);
					}
				}
				// Typed characters are echoed by the screen reader itself.
				continue;
			}
			if(f.r == role::long_text) {
				said.push_back(label_of(*static_cast<styled_widget*>(f.w)));
				continue;
			}
			std::string v = control_value(f, false);
			if(f.w != st.focus) {
				v = join({control_name(f), v});
			}
			// A button only changes by becoming available or unavailable.
			v = join({v, control_active(f) ? (control_value(f, false).empty() ? std::string(_("available")) : std::string())
											: std::string(_("unavailable"))});
			said.push_back(v);
		}
	}

	// Changed primary content (story text, message, description of the selected item).
	for(const text_item& t : c.texts) {
		if(!t.content) {
			continue;
		}
		auto old = st.content.find(t.w);
		if(old != st.content.end() && old->second == t.text) {
			continue;
		}
		if(old == st.content.end() && !from_key && st.announced) {
			// A content text that just appeared (e.g. story text fading in).
		}
		// The same text may already have been said as a focused long text.
		if(std::find(said.begin(), said.end(), t.text) == said.end()) {
			said.push_back(t.text);
		}
		if(win.id() == "story_viewer") {
			std::string speaker;
			if(label* tl = find_widget<label>(&win, "title", false, false)) {
				speaker = label_of(*tl);
			}
			add_dialogue(speaker, t.text);
		}
	}

	snapshot(win, c, st);
	if(!said.empty()) {
		speak(join(said, ". "), from_key ? msg::nav : msg::event);
	}
}

bool activate(window& win, window_state& st, const focusable& f, bool space)
{
	if(!control_active(f)) {
		speak(control_name(f) + ", " + _("unavailable"), msg::reply);
		cue("error");
		return true;
	}
	switch(f.r) {
	case role::button:
	case role::menu:
	case role::multimenu:
		debug("gui activate: " + f.w->id());
		f.w->fire(event::LEFT_BUTTON_CLICK, *f.w);
		return true;
	case role::check:
	case role::sort_header:
	case role::toggle_panel:
		if(!space && f.r == role::toggle_panel) {
			return false; // Enter keeps its native meaning (often OK).
		}
		if(!space && f.r == role::check) {
			return false;
		}
		f.w->fire(event::LEFT_BUTTON_CLICK, *f.w);
		return true;
	case role::list:
		if(space) {
			// Toggle the first check box in the selected row (mod lists etc.).
			auto* l = static_cast<listbox*>(f.w);
			int sel = l->get_selected_row();
			if(sel >= 0) {
				if(grid* g = l->get_row_grid(sel)) {
					collection rc;
					std::string none;
					collect_grid(*g, rc, none);
					for(const auto& rf : rc.controls) {
						if(rf.r == role::check && control_active(rf)) {
							rf.w->fire(event::LEFT_BUTTON_CLICK, *rf.w);
							speak(control_value(rf, false), msg::nav);
							return true;
						}
					}
				}
			}
		}
		return false;
	default:
		return false;
	}
	(void)win;
	(void)st;
}

/** Line based review of long texts with the arrow keys. */
bool review_long_text(window_state& st, const focusable& f, SDL_Keycode key)
{
	std::string text = label_of(*static_cast<styled_widget*>(f.w));
	// Split into sentences/lines.
	std::vector<std::string> lines;
	std::string cur;
	for(std::size_t i = 0; i < text.size(); ++i) {
		cur += text[i];
		bool end = (text[i] == '.' || text[i] == '!' || text[i] == '?') && (i + 1 == text.size() || text[i + 1] == ' ');
		if(end) {
			std::string t = plain_text(cur);
			if(!t.empty()) {
				lines.push_back(t);
			}
			cur.clear();
		}
	}
	if(!plain_text(cur).empty()) {
		lines.push_back(plain_text(cur));
	}
	if(lines.empty()) {
		return false;
	}
	int n = static_cast<int>(lines.size());
	int pos = static_cast<int>(st.text_line);
	switch(key) {
	case SDLK_DOWN: pos = std::min(n - 1, pos + 1); break;
	case SDLK_UP: pos = std::max(0, pos - 1); break;
	case SDLK_HOME: pos = 0; break;
	case SDLK_END: pos = n - 1; break;
	default: return false;
	}
	if(pos == static_cast<int>(st.text_line) && (key == SDLK_DOWN || key == SDLK_UP)) {
		cue("edge");
	}
	st.text_line = pos;
	speak(lines[pos], msg::nav);
	return true;
}

bool is_printable(const SDL_Event& event)
{
	const SDL_Keycode k = event.key.keysym.sym;
	const unsigned mod = event.key.keysym.mod;
	return !(mod & (KMOD_CTRL | KMOD_ALT)) && ((k >= 32 && k < 127));
}

} // anonymous namespace

bool key_before(window& win, const SDL_Event& event)
{
	window_state& st = state_of(win);

	if(event.type == SDL_TEXTINPUT) {
		if(st.suppress_text_input) {
			st.suppress_text_input = false;
			return true;
		}
		return false;
	}
	if(event.type != SDL_KEYDOWN) {
		return false;
	}
	st.suppress_text_input = false;
	st.last_key_ticks = SDL_GetTicks();

	if(handle_global_key(event)) {
		if(is_printable(event)) {
			st.suppress_text_input = true;
		}
		return true;
	}
	if(!enabled() || win.mode() != window::show_mode::modal) {
		return false;
	}

	collection c = collect_window(win);
	snapshot(win, c, st);
	int idx = index_of(c, st.focus);
	if(idx < 0) {
		idx = initial_focus(win, c);
		if(idx >= 0) {
			set_focus(win, st, c, idx, false);
		}
	}

	command cmd = keymap::lookup(context::dialog, event);
	const SDL_Keycode key = event.key.keysym.sym;
	const unsigned mod = event.key.keysym.mod;
	const bool plain = !(mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT));

	if(cmd == command::gui_next || cmd == command::gui_prev) {
		if(c.controls.empty()) {
			speak(read_all_text(win), msg::reply);
			return true;
		}
		int n = static_cast<int>(c.controls.size());
		int next = idx;
		for(int tries = 0; tries < n; ++tries) {
			next = (next + (cmd == command::gui_next ? 1 : -1) + n) % n;
			// Skip unavailable controls only if some control is available.
			break;
		}
		if(next == idx && n > 1) {
			next = (idx + 1) % n;
		}
		if(cmd == command::gui_next && next <= idx) {
			cue("edge");
		} else if(cmd == command::gui_prev && next >= idx) {
			cue("edge");
		}
		set_focus(win, st, c, next, true);
		snapshot(win, c, st);
		return true;
	}
	if(cmd == command::gui_read_all) {
		speak(read_all_text(win), msg::reply);
		return true;
	}
	if(cmd == command::gui_describe) {
		if(idx >= 0) {
			const focusable& f = c.controls[idx];
			std::string extra;
			if(auto* sw = dynamic_cast<styled_widget*>(f.w)) {
				extra = tooltip_of(*sw);
			}
			speak(join({describe(f), extra, std::to_string(idx + 1) + " of " + std::to_string(c.controls.size())}), msg::reply);
		} else {
			speak(_("No controls"), msg::reply);
		}
		return true;
	}

	if(idx < 0) {
		return false;
	}
	const focusable& f = c.controls[idx];

	if(plain && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE)) {
		bool space = key == SDLK_SPACE;
		if(f.r == role::text || f.r == role::password || f.r == role::spin) {
			return false; // native handling (space types, enter submits)
		}
		if(activate(win, st, f, space)) {
			if(space) {
				st.suppress_text_input = true;
			}
			// The click handlers ran synchronously: report what they changed
			// (unless they closed this window).
			if(std::find(g_stack.begin(), g_stack.end(), &win) != g_stack.end()) {
				announce_changes(win, state_of(win), true);
			}
			return true;
		}
		return false;
	}

	if(f.r == role::long_text && plain) {
		if(review_long_text(st, f, key)) {
			return true;
		}
	}

	if(f.r == role::button && plain && !st.review_lines.empty()
		&& (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_HOME || key == SDLK_END)) {
		// Lines of a window without a text control (the damage calculation).
		const int n = static_cast<int>(st.review_lines.size());
		int pos = st.review_pos;
		if(key == SDLK_DOWN) {
			pos = pos + 1;
		} else if(key == SDLK_UP) {
			pos = pos - 1;
		} else if(key == SDLK_HOME) {
			pos = 0;
		} else {
			pos = n - 1;
		}
		if(pos < 0 || pos >= n) {
			cue("edge");
			pos = std::clamp(pos, 0, n - 1);
		}
		st.review_pos = pos;
		speak(st.review_lines[pos], msg::nav);
		return true;
	}

	if(f.r == role::list && plain && (key == SDLK_HOME || key == SDLK_END)) {
		// First / last item of a list (GUI2 lists have no Home/End keys of their own).
		auto* l = static_cast<listbox*>(f.w);
		generator_base* gen = list_generator(*l);
		const unsigned n = l->get_item_count();
		int target = -1;
		for(unsigned ordered = 0; ordered < n; ++ordered) {
			unsigned idx = gen ? gen->get_item_at_ordered(ordered) : ordered;
			if(idx >= n || (gen && !gen->get_item_shown(idx))) {
				continue;
			}
			if(key == SDLK_END || target < 0) {
				target = static_cast<int>(idx);
			}
		}
		if(target >= 0) {
			if(target == l->get_selected_row()) {
				cue("edge");
			}
			l->select_row(target);
			l->fire(event::NOTIFY_MODIFIED, *l, nullptr);
		}
		announce_changes(win, st, true);
		return true;
	}

	if(f.r == role::menu && plain && (key == SDLK_UP || key == SDLK_DOWN)) {
		auto* m = static_cast<menu_button*>(f.w);
		if(!m->get_active() || m->num_states() == 0) {
			return true;
		}
		int v = static_cast<int>(m->get_value()) + (key == SDLK_DOWN ? 1 : -1);
		if(v < 0 || v >= static_cast<int>(m->num_states())) {
			cue("edge");
			v = std::clamp(v, 0, static_cast<int>(m->num_states()) - 1);
		}
		m->set_selected(v, true);
		return false; // key_after announces the new value
	}

	return false;
}

void key_after(window& win, const SDL_Event& event)
{
	if(!enabled() || event.type != SDL_KEYDOWN || win.mode() != window::show_mode::modal) {
		return;
	}
	if(std::find(g_stack.begin(), g_stack.end(), &win) == g_stack.end()) {
		return; // closed during the key
	}
	announce_changes(win, state_of(win), true);
}

void window_tick(window& win)
{
	if(!enabled() || win.mode() != window::show_mode::modal) {
		return;
	}
	auto it = g_windows.find(&win);
	if(it == g_windows.end()) {
		return;
	}
	window_state& st = it->second;
	uint32_t now = SDL_GetTicks();
	if(now - st.last_poll_ticks < 300) {
		return;
	}
	st.last_poll_ticks = now;
	if(now - st.last_key_ticks < 300) {
		return;
	}
	if(!st.announced) {
		return;
	}
	announce_changes(win, st, false);
}

} // namespace accessibility::gui
