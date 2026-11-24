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

#include "accessibility/game_access.hpp"

#include "accessibility/accessibility.hpp"
#include "accessibility/gui_access.hpp"
#include "accessibility/keymap.hpp"

#include "actions/attack.hpp"
#include "controller_base.hpp"
#include "formula/string_utils.hpp"
#include "game_board.hpp"
#include "game_display.hpp"
#include "gettext.hpp"
#include "gui/dialogs/edit_text.hpp"
#include "map/label.hpp"
#include "map/map.hpp"
#include "menu_events.hpp"
#include "mouse_events.hpp"
#include "overlay.hpp"
#include "pathfind/pathfind.hpp"
#include "play_controller.hpp"
#include "reports.hpp"
#include "resources.hpp"
#include "serialization/string_utils.hpp"
#include "team.hpp"
#include "tod_manager.hpp"
#include "units/map.hpp"
#include "units/unit.hpp"
#include "units/types.hpp"
#include "utils/general.hpp"

#include <SDL2/SDL_events.h>
#include <SDL2/SDL_timer.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>

namespace accessibility::game
{
namespace
{
// ---------------------------------------------------------------- state ---

struct game_state_access
{
	play_controller* pc = nullptr;
	map_location cursor;
	map_location previous;
	std::array<map_location, 10> bookmarks;
	/** Index into the last list cycled through, per list kind. */
	std::map<int, int> list_pos;
	/** Attack plan chosen with F7: attack @a target from @a from. */
	map_location plan_target;
	map_location plan_from;
	/** Enemy turn summary. */
	std::vector<std::string> other_turn_log;
	int other_turn_side = 0;
	bool suppress_text_input = false;
	bool in_combat = false;
	/** Healing results collected at the start of a side turn. */
	std::vector<std::string> heal_lines;
	map_location last_selected;
	std::string last_selected_id;
	uint32_t last_escape = 0;
	bool combat_visible = false;
	std::vector<std::string> combat_lines;
};

game_state_access g;

bool active()
{
	return enabled() && g.pc != nullptr && !resources::simulation_;
}

game_display& disp()
{
	return g.pc->get_display();
}

const gamemap& gmap()
{
	return g.pc->get_map();
}

const game_board& board()
{
	return g.pc->gamestate().board_;
}

const team& viewer()
{
	return board().teams()[disp().viewing_team()];
}

int viewing_side()
{
	return disp().viewing_side();
}

events::mouse_handler& mouse()
{
	return g.pc->get_mouse_handler_base();
}

std::string coords(const map_location& loc)
{
	return std::to_string(loc.wml_x()) + ", " + std::to_string(loc.wml_y());
}

bool on_board(const map_location& loc)
{
	return gmap().on_board(loc);
}

/** Visible unit at @a loc for the viewing team (fog, shroud and invisibility respected). */
const unit* visible_unit(const map_location& loc)
{
	if(!on_board(loc)) {
		return nullptr;
	}
	const display_context& dc = board();
	return dc.get_visible_unit(loc, viewer(), disp().show_everything());
}

bool loc_shrouded(const map_location& loc)
{
	return disp().shrouded(loc);
}

bool loc_fogged(const map_location& loc)
{
	return disp().fogged(loc);
}

const unit* selected_unit()
{
	map_location sel = mouse().get_selected_hex();
	return visible_unit(sel);
}

// ------------------------------------------------------- native reports ---

struct report_result
{
	std::string text;
	std::string tooltip;
};

report_result report(const std::string& name)
{
	report_result res;
	try {
		reports r;
		reports::context ctx(board(), disp(), g.pc->get_tod_manager(), g.pc->get_whiteboard(), mouse());
		config cfg = r.generate_report(name, ctx);
		std::vector<std::string> texts, tips;
		for(const config& el : cfg.child_range("element")) {
			std::string t = el["text"].str();
			std::string tip = el["tooltip"].str();
			if(!t.empty()) {
				texts.push_back(t);
			}
			if(!tip.empty() && (tips.empty() || tips.back() != tip)) {
				tips.push_back(tip);
			}
		}
		auto clean = [](std::string s) {
			// Line breaks become sentence breaks.
			std::string out;
			for(char c : s) {
				if(c == '\n') {
					if(!out.empty() && out.back() != '.' && out.back() != ':' && out.back() != ' ') {
						out += '.';
					}
					out += ' ';
				} else {
					out += c;
				}
			}
			return plain_text(out);
		};
		res.text = clean(join(texts, " "));
		res.tooltip = clean(join(tips, "\n"));
	} catch(const std::exception& e) {
		log(std::string("report ") + name + " failed: " + e.what());
	}
	return res;
}

// ---------------------------------------------------------- descriptions ---

std::string relation(const unit& u)
{
	if(u.side() == viewing_side()) {
		return _("your");
	}
	if(viewer().is_enemy(u.side())) {
		return _("enemy");
	}
	return _("allied");
}

std::string unit_title(const unit& u)
{
	std::string type = u.type_name().str();
	std::string name = u.name().str();
	std::string res = relation(u) + " " + type;
	if(!name.empty() && name != type) {
		res += " " + name;
	}
	if(u.can_recruit()) {
		res += ", " + std::string(_("leader"));
	}
	return res;
}

std::vector<std::string> status_list(const unit& u)
{
	std::vector<std::string> st;
	if(u.get_state(unit::STATE_POISONED)) st.push_back(_("poisoned"));
	if(u.get_state(unit::STATE_SLOWED)) st.push_back(_("slowed"));
	if(u.get_state(unit::STATE_PETRIFIED)) st.push_back(_("petrified"));
	if(u.get_state(unit::STATE_UNHEALABLE)) st.push_back(_("unhealable"));
	if(u.get_state(unit::STATE_GUARDIAN)) st.push_back(_("guardian"));
	if(u.get_state(unit::STATE_INVULNERABLE)) st.push_back(_("invulnerable"));
	return st;
}

std::string hp_text(const unit& u)
{
	return std::to_string(u.hitpoints()) + " " + _("of") + " " + std::to_string(u.max_hitpoints()) + " " + _("hit points");
}

std::string actions_left(const unit& u)
{
	if(u.side() != viewing_side()) {
		return std::string();
	}
	std::vector<std::string> parts;
	parts.push_back(std::to_string(u.movement_left()) + " " + _("of") + " " + std::to_string(u.total_movement()) + " " + _("moves"));
	if(u.attacks_left() == 0) {
		parts.push_back(_("no attack left"));
	}
	if(u.user_end_turn()) {
		parts.push_back(_("turn finished"));
	}
	return join(parts);
}

/** Short description used while moving the cursor. */
std::string unit_brief(const unit& u)
{
	std::vector<std::string> parts {unit_title(u), hp_text(u)};
	int verbosity = prefs().verbosity;
	if(verbosity >= 1) {
		parts.push_back(actions_left(u));
		auto st = status_list(u);
		parts.insert(parts.end(), st.begin(), st.end());
	}
	if(verbosity >= 2) {
		parts.push_back(_("level") + std::string(" ") + std::to_string(u.level()));
		parts.push_back(_("experience") + std::string(" ") + std::to_string(u.experience()) + " " + _("of") + " " + std::to_string(u.max_experience()));
	}
	return join(parts);
}

std::string side_label(int side)
{
	if(side <= 0 || side > static_cast<int>(board().teams().size())) {
		return std::string();
	}
	const team& t = board().get_team(side);
	std::string name = plain_text(t.side_name());
	std::string res = _("side") + std::string(" ") + std::to_string(side);
	if(!name.empty()) {
		res += " " + name;
	}
	return res;
}

std::string village_text(const map_location& loc)
{
	if(!gmap().is_village(loc)) {
		return std::string();
	}
	int owner = board().village_owner(loc);
	if(viewer().fogged(loc)) {
		return _("village, owner unknown under fog");
	}
	if(owner == 0) {
		return _("unowned village");
	}
	if(owner == viewing_side()) {
		return _("your village");
	}
	if(viewer().is_enemy(owner)) {
		return _("enemy village") + std::string(", ") + side_label(owner);
	}
	return _("allied village") + std::string(", ") + side_label(owner);
}

std::string label_text(const map_location& loc)
{
	// map_labels::get_label() returns the viewing team's label or a global one,
	// which is what the display shows; fog and shroud can hide labels.
	const terrain_label* l = disp().labels().get_label(loc);
	if(!l || (!l->visible_in_fog() && loc_fogged(loc)) || (!l->visible_in_shroud() && loc_shrouded(loc))) {
		return std::string();
	}
	std::string t = plain_text(l->text().str());
	return t.empty() ? std::string() : _("label") + std::string(" ") + t;
}

std::vector<std::string> item_texts(const map_location& loc)
{
	std::vector<std::string> res;
	const auto& ovs = disp().access_overlays();
	auto it = ovs.find(loc);
	if(it == ovs.end() || loc_shrouded(loc)) {
		return res;
	}
	for(const overlay& ov : it->second) {
		bool visible = true;
		if(!disp().show_everything() && !ov.team_name.empty()) {
			const std::vector<std::string> mine = utils::split(viewer().team_name());
			const std::vector<std::string> theirs = utils::split(ov.team_name);
			visible = std::find_first_of(theirs.begin(), theirs.end(), mine.begin(), mine.end()) != theirs.end();
		}
		if(!visible || (loc_fogged(loc) && !ov.visible_in_fog)) {
			continue;
		}
		std::string name = plain_text(ov.name.str());
		if(name.empty()) {
			// Derive a name from the image file: "items/chest-closed.png~..." -> "chest closed".
			std::string img = ov.image.substr(0, ov.image.find('~'));
			auto slash = img.find_last_of('/');
			if(slash != std::string::npos) {
				img = img.substr(slash + 1);
			}
			auto dot = img.find('.');
			if(dot != std::string::npos) {
				img = img.substr(0, dot);
			}
			std::replace(img.begin(), img.end(), '-', ' ');
			std::replace(img.begin(), img.end(), '_', ' ');
			name = img;
		}
		if(!name.empty()) {
			res.push_back(_("item") + std::string(" ") + name);
		}
	}
	return res;
}

/** Defense, movement cost and reachability for the selected unit. */
std::string selected_unit_terrain_info(const map_location& loc, bool include_reach)
{
	const unit* sel = selected_unit();
	if(!sel || loc_shrouded(loc)) {
		return std::string();
	}
	const t_translation::terrain_code terrain = gmap().get_terrain(loc);
	int cost = sel->movement_cost(terrain);
	int defense = 100 - sel->defense_modifier(terrain);
	std::vector<std::string> parts;
	if(prefs().terrain_detail >= 1) {
		parts.push_back(std::to_string(defense) + "% " + _("defense"));
		if(cost >= movetype::UNREACHABLE) {
			parts.push_back(_("impassable"));
		} else {
			parts.push_back(_("move cost") + std::string(" ") + std::to_string(cost));
		}
	}
	if(include_reach && sel->side() == viewing_side() && loc != sel->get_location()) {
		const pathfind::paths& paths = mouse().current_paths();
		auto dest = paths.destinations.find(loc);
		if(dest != paths.destinations.end()) {
			parts.push_back(_("reachable") + std::string(", ") + std::to_string(dest->move_left) + " " + _("moves left"));
		} else if(const unit* there = visible_unit(loc); there && viewer().is_enemy(there->side())) {
			// An enemy is attackable if some reachable hex next to it is free.
			bool can_attack = sel->attacks_left() > 0;
			bool adjacent_reachable = false;
			for(const map_location& adj : get_adjacent_tiles(loc)) {
				if(adj == sel->get_location() || (paths.destinations.contains(adj) && !visible_unit(adj))) {
					adjacent_reachable = true;
					break;
				}
			}
			if(can_attack && adjacent_reachable) {
				parts.push_back(_("can be attacked"));
			}
		} else if(!paths.destinations.empty()) {
			parts.push_back(_("out of reach this turn"));
		}
	}
	return join(parts);
}

std::string terrain_name(const map_location& loc)
{
	return plain_text(gmap().get_terrain_string(loc));
}

/** Tile description: what a sighted player sees on the hex. */
std::string describe_tile(const map_location& loc, int detail, bool with_coords)
{
	if(!on_board(loc)) {
		return _("off the map");
	}
	std::vector<std::string> parts;
	if(loc_shrouded(loc)) {
		parts.push_back(_("unexplored"));
		if(with_coords) {
			parts.push_back(coords(loc));
		}
		return join(parts);
	}
	if(const unit* u = visible_unit(loc)) {
		parts.push_back(detail >= 1 ? unit_brief(*u) : unit_title(*u) + ", " + hp_text(*u));
	}
	parts.push_back(terrain_name(loc));
	parts.push_back(village_text(loc));
	if(loc_fogged(loc)) {
		parts.push_back(_("fogged"));
	}
	parts.push_back(label_text(loc));
	auto items = item_texts(loc);
	parts.insert(parts.end(), items.begin(), items.end());
	if(detail >= 1) {
		parts.push_back(selected_unit_terrain_info(loc, true));
	}
	if(detail >= 2) {
		const t_translation::terrain_code terrain = gmap().get_terrain(loc);
		const terrain_type& info = gmap().get_terrain_info(terrain);
		if(info.gives_healing() > 0) {
			parts.push_back(_("heals") + std::string(" ") + std::to_string(info.gives_healing()));
		}
		const time_of_day& tod = g.pc->get_tod_manager().get_time_of_day(loc);
		parts.push_back(plain_text(tod.name.str()));
	}
	if(with_coords) {
		parts.push_back(coords(loc));
	}
	return join(parts);
}

// ------------------------------------------------------- cursor movement ---

void sync_hover(const map_location& loc)
{
	if(!on_board(loc)) {
		return;
	}
	disp().scroll_to_tile(loc, display::ONSCREEN, false, false);
	mouse().access_hover(loc, g.pc->is_browsing());
	if(mouse().get_last_hex() != loc) {
		log("hover not applied: wanted " + coords(loc) + ", native hover is " + coords(mouse().get_last_hex()));
	}
}

void set_cursor(map_location loc, bool announce, const std::string& prefix = std::string())
{
	if(!on_board(loc)) {
		return;
	}
	if(loc != g.cursor) {
		g.previous = g.cursor;
	}
	g.cursor = loc;
	sync_hover(loc);
	if(announce) {
		std::string text = describe_tile(loc, prefs().verbosity >= 1 ? 1 : 0, prefs().coordinates);
		speak(prefix.empty() ? text : prefix + ". " + text, msg::nav);
	}
}

void move_cursor(map_location::DIRECTION dir, bool jump)
{
	map_location next = g.cursor.get_direction(dir);
	if(!on_board(next)) {
		cue("edge");
		speak(_("edge of map"), msg::nav);
		return;
	}
	if(jump) {
		// Move until the terrain changes, a unit appears, or the map ends.
		const t_translation::terrain_code start = loc_shrouded(next) ? t_translation::terrain_code() : gmap().get_terrain(next);
		const bool start_shroud = loc_shrouded(next);
		int steps = 1;
		for(;;) {
			if(visible_unit(next) || gmap().is_village(next) || label_text(next) != "") {
				break;
			}
			map_location after = next.get_direction(dir);
			if(!on_board(after)) {
				break;
			}
			bool after_shroud = loc_shrouded(after);
			if(after_shroud != start_shroud || (!after_shroud && gmap().get_terrain(after) != start)) {
				break;
			}
			next = after;
			++steps;
		}
		set_cursor(next, true, std::to_string(steps) + " " + _("steps"));
		return;
	}
	if(prefs().audio_cues && visible_unit(next)) {
		cue("move");
	}
	set_cursor(next, true);
}

map_location::DIRECTION dir_of(command c)
{
	switch(c) {
	case command::move_n: case command::jump_n: return map_location::NORTH;
	case command::move_ne: case command::jump_ne: return map_location::NORTH_EAST;
	case command::move_se: case command::jump_se: return map_location::SOUTH_EAST;
	case command::move_s: case command::jump_s: return map_location::SOUTH;
	case command::move_sw: case command::jump_sw: return map_location::SOUTH_WEST;
	case command::move_nw: case command::jump_nw: return map_location::NORTH_WEST;
	default: return map_location::NDIRECTIONS;
	}
}

// ------------------------------------------------------------- lists ---

enum list_kind { L_UNITS, L_ACTIVE, L_WOUNDED, L_ALLIES, L_ENEMIES, L_VILLAGES, L_LABELS, L_REACH, L_TARGETS };

struct list_entry
{
	map_location loc;
	map_location from; /**< Attack position for L_TARGETS. */
	std::string text;
};

bool loc_less(const map_location& a, const map_location& b)
{
	return a.y != b.y ? a.y < b.y : a.x < b.x;
}

std::vector<list_entry> build_list(list_kind kind)
{
	std::vector<list_entry> res;
	const team& me = viewer();
	switch(kind) {
	case L_UNITS:
	case L_ACTIVE:
	case L_WOUNDED:
	case L_ALLIES:
	case L_ENEMIES: {
		std::vector<const unit*> units;
		for(const unit& u : board().units()) {
			const map_location& loc = u.get_location();
			if(visible_unit(loc) != &u) {
				continue;
			}
			bool mine = u.side() == viewing_side();
			bool enemy = me.is_enemy(u.side());
			switch(kind) {
			case L_UNITS: if(!mine) continue; break;
			case L_ACTIVE:
				if(!mine || u.user_end_turn() || u.incapacitated()) continue;
				if(u.movement_left() == 0 && u.attacks_left() == 0) continue;
				break;
			case L_WOUNDED: if(!mine || u.hitpoints() >= u.max_hitpoints()) continue; break;
			case L_ALLIES: if(mine || enemy) continue; break;
			case L_ENEMIES: if(!enemy) continue; break;
			default: break;
			}
			units.push_back(&u);
		}
		std::sort(units.begin(), units.end(), [](const unit* a, const unit* b) {
			if(a->can_recruit() != b->can_recruit()) {
				return a->can_recruit();
			}
			return loc_less(a->get_location(), b->get_location());
		});
		for(const unit* u : units) {
			res.push_back({u->get_location(), map_location(), unit_brief(*u)});
		}
		break;
	}
	case L_VILLAGES: {
		std::vector<map_location> vs = gmap().villages();
		std::sort(vs.begin(), vs.end(), loc_less);
		for(const map_location& v : vs) {
			if(loc_shrouded(v)) {
				continue;
			}
			res.push_back({v, map_location(), join({village_text(v), terrain_name(v)})});
		}
		break;
	}
	case L_LABELS: {
		for(int y = 0; y < gmap().h(); ++y) {
			for(int x = 0; x < gmap().w(); ++x) {
				map_location loc(x, y);
				std::string t = label_text(loc);
				if(!t.empty()) {
					res.push_back({loc, map_location(), t});
				}
			}
		}
		break;
	}
	case L_REACH: {
		const unit* sel = selected_unit();
		if(!sel) {
			break;
		}
		const pathfind::paths& paths = mouse().current_paths();
		std::vector<pathfind::paths::step> steps(paths.destinations.begin(), paths.destinations.end());
		std::sort(steps.begin(), steps.end(), [](const pathfind::paths::step& a, const pathfind::paths::step& b) {
			if(a.move_left != b.move_left) {
				return a.move_left > b.move_left;
			}
			return loc_less(a.curr, b.curr);
		});
		for(const auto& s : steps) {
			if(s.curr == sel->get_location() || visible_unit(s.curr)) {
				continue;
			}
			res.push_back({s.curr, map_location(),
				join({terrain_name(s.curr), village_text(s.curr), std::to_string(s.move_left) + " " + _("moves left")})});
		}
		break;
	}
	case L_TARGETS: {
		const unit* sel = selected_unit();
		if(!sel || sel->side() != viewing_side() || sel->attacks_left() == 0) {
			break;
		}
		const pathfind::paths& paths = mouse().current_paths();
		for(const unit& e : board().units()) {
			const map_location& eloc = e.get_location();
			if(visible_unit(eloc) != &e || !me.is_enemy(e.side()) || e.incapacitated()) {
				continue;
			}
			for(const map_location& adj : get_adjacent_tiles(eloc)) {
				if(!on_board(adj)) {
					continue;
				}
				bool own_hex = adj == sel->get_location();
				if(!own_hex && (!paths.destinations.contains(adj) || visible_unit(adj))) {
					continue;
				}
				const t_translation::terrain_code terrain = gmap().get_terrain(adj);
				std::string from = own_hex ? std::string(_("from your current position"))
					: _("from") + std::string(" ") + coords(adj) + ", " + terrain_name(adj) + ", "
						+ std::to_string(100 - sel->defense_modifier(terrain)) + "% " + _("defense");
				res.push_back({eloc, adj, unit_brief(e) + ", " + coords(eloc) + ", " + from});
			}
		}
		std::sort(res.begin(), res.end(), [](const list_entry& a, const list_entry& b) {
			if(a.loc != b.loc) return loc_less(a.loc, b.loc);
			return loc_less(a.from, b.from);
		});
		break;
	}
	}
	return res;
}

std::string list_name(list_kind kind)
{
	switch(kind) {
	case L_UNITS: return _("your units");
	case L_ACTIVE: return _("units that can still act");
	case L_WOUNDED: return _("wounded units");
	case L_ALLIES: return _("allied units");
	case L_ENEMIES: return _("visible enemies");
	case L_VILLAGES: return _("villages");
	case L_LABELS: return _("labels");
	case L_REACH: return _("reachable destinations");
	case L_TARGETS: return _("attack options");
	}
	return std::string();
}

void cycle_list(list_kind kind, int direction)
{
	std::vector<list_entry> items = build_list(kind);
	if(items.empty()) {
		std::string why;
		if((kind == L_REACH || kind == L_TARGETS) && !selected_unit()) {
			why = _("Select a unit first with Enter.");
		}
		speak(join({_("No") + std::string(" ") + list_name(kind), why}, ". "), msg::reply);
		cue("error");
		return;
	}
	int n = static_cast<int>(items.size());
	int pos = -1;
	// Continue from the cursor position if it is in the list.
	auto it = g.list_pos.find(kind);
	if(it != g.list_pos.end() && it->second >= 0 && it->second < n && items[it->second].loc == g.cursor
		&& (kind != L_TARGETS || items[it->second].from == g.plan_from)) {
		pos = it->second;
	}
	if(pos < 0) {
		pos = direction > 0 ? -1 : n;
	}
	pos += direction;
	if(pos >= n) {
		pos = 0;
		cue("edge");
	} else if(pos < 0) {
		pos = n - 1;
		cue("edge");
	}
	g.list_pos[kind] = pos;
	const list_entry& e = items[pos];
	if(kind == L_TARGETS) {
		g.plan_target = e.loc;
		g.plan_from = e.from;
	}
	if(e.loc != g.cursor) {
		g.previous = g.cursor;
	}
	g.cursor = e.loc;
	sync_hover(e.loc);
	std::string text = e.text + ". " + std::to_string(pos + 1) + " " + _("of") + " " + std::to_string(n);
	if(kind != L_TARGETS && prefs().coordinates) {
		text += ". " + coords(e.loc);
	}
	if(kind == L_TARGETS) {
		text += ". " + std::string(_("Press Enter to open the attack dialog."));
	}
	speak(text, msg::nav);
}

// ------------------------------------------------------------- actions ---

void select_or_act()
{
	if(!on_board(g.cursor)) {
		return;
	}
	events::mouse_handler& mh = mouse();
	const bool browse = g.pc->is_browsing();
	const unit* sel = selected_unit();

	// Attack plan from F7: hover the chosen attack position first so the
	// native code picks it as the attack direction.
	if(sel && g.plan_target == g.cursor && g.plan_from.valid() && visible_unit(g.cursor)) {
		if(g.plan_from != sel->get_location()) {
			mh.access_hover(g.plan_from, browse);
		}
	}
	mh.access_hover(g.cursor, browse);
	debug("select_or_action at " + coords(g.cursor));

	const map_location before_sel = mh.get_selected_hex();
	const unit* target = visible_unit(g.cursor);
	bool was_move = sel && sel->side() == viewing_side() && !target && g.cursor != sel->get_location()
		&& mh.current_paths().destinations.contains(g.cursor);

	if(browse && sel && !target) {
		// Not our turn: native code only allows browsing.
	}
	mh.select_or_action(browse);
	g.plan_target = map_location();
	g.plan_from = map_location();

	// Moves and attacks announce themselves through the action hooks; report
	// nothing happened when the click had no effect.
	if(!was_move && !target && mh.get_selected_hex() == before_sel && sel) {
		if(!mh.current_paths().destinations.contains(g.cursor)) {
			speak(_("Cannot move there."), msg::reply);
			cue("error");
		}
	}
}

void cancel_selection()
{
	events::mouse_handler& mh = mouse();
	if(mh.get_selected_hex().valid()) {
		mh.deselect_hex();
		speak(_("Selection cleared."), msg::reply);
	}
}

void goto_coordinates()
{
	std::string text;
	if(!gui2::dialogs::edit_text::execute(_("Jump to coordinates"),
		   _("Enter column and row, for example 12 5:"), text)) {
		return;
	}
	std::vector<int> nums;
	std::string cur;
	for(char c : text + " ") {
		if(std::isdigit(static_cast<unsigned char>(c))) {
			cur += c;
		} else if(!cur.empty()) {
			nums.push_back(std::atoi(cur.c_str()));
			cur.clear();
		}
	}
	if(nums.size() < 2) {
		speak(_("Please enter two numbers."), msg::reply);
		cue("error");
		return;
	}
	map_location loc(nums[0], nums[1], wml_loc());
	if(!on_board(loc)) {
		speak(_("Those coordinates are outside the map."), msg::reply);
		cue("error");
		return;
	}
	set_cursor(loc, true);
}

std::string distance_text(int d)
{
	return std::to_string(d) + " " + (d == 1 ? _("hex") : _("hexes"));
}

void where_am_i()
{
	std::vector<std::string> parts {coords(g.cursor)};
	if(const unit* sel = selected_unit()) {
		if(sel->get_location() != g.cursor) {
			parts.push_back(relative_position(sel->get_location(), g.cursor) + " " + _("from the selected unit"));
		}
	}
	const unit* leader = nullptr;
	for(const unit& u : board().units()) {
		if(u.side() == viewing_side() && u.can_recruit()) {
			leader = &u;
			break;
		}
	}
	if(leader && leader->get_location() != g.cursor) {
		parts.push_back(relative_position(leader->get_location(), g.cursor) + " " + _("from your leader"));
	}
	speak(join(parts, ". "), msg::reply);
}

void map_info()
{
	std::vector<std::string> parts;
	parts.push_back(plain_text(g.pc->get_scenario_name().str()));
	parts.push_back(_("map") + std::string(" ") + std::to_string(gmap().w()) + " " + _("columns by") + " " + std::to_string(gmap().h()) + " " + _("rows"));
	parts.push_back(_("cursor at") + std::string(" ") + coords(g.cursor));
	speak(join(parts, ". "), msg::reply);
}

void describe_adjacent()
{
	std::vector<std::string> parts;
	for(int d = 0; d < map_location::NDIRECTIONS; ++d) {
		auto dir = static_cast<map_location::DIRECTION>(d);
		map_location adj = g.cursor.get_direction(dir);
		parts.push_back(direction_name(dir) + ": " + (on_board(adj) ? describe_tile(adj, 0, false) : std::string(_("off the map"))));
	}
	speak(join(parts, ". "), msg::reply);
}

void describe_route()
{
	const unit* sel = selected_unit();
	if(!sel) {
		speak(_("No unit selected."), msg::reply);
		return;
	}
	if(sel->get_location() == g.cursor) {
		speak(_("The cursor is on the selected unit."), msg::reply);
		return;
	}
	team& t = g.pc->get_teams()[disp().viewing_team()];
	pathfind::marked_route route = mouse().get_route(sel, g.cursor, t);
	if(route.steps.empty()) {
		speak(_("No route to this location."), msg::reply);
		return;
	}
	std::vector<std::string> parts;
	const std::size_t hexes = route.steps.size() - 1;
	parts.push_back(distance_text(static_cast<int>(hexes)) + " " + _("along the route"));
	int turns = 0;
	for(const auto& m : route.marks) {
		turns = std::max(turns, m.second.turns);
	}
	if(mouse().current_paths().destinations.contains(g.cursor)) {
		parts.push_back(_("reachable this turn"));
	} else if(turns > 0) {
		parts.push_back(VNGETTEXT("takes $n turn", "takes $n turns", turns, {{"n", std::to_string(turns)}}));
	}
	if(route.move_cost < movetype::UNREACHABLE) {
		parts.push_back(_("movement cost") + std::string(" ") + std::to_string(route.move_cost));
	}
	// Step list (directions), compressed: "2 north, 1 north-east".
	std::vector<std::string> legs;
	map_location::DIRECTION last = map_location::NDIRECTIONS;
	int count = 0;
	for(std::size_t i = 1; i < route.steps.size(); ++i) {
		auto dir = route.steps[i - 1].get_relative_dir(route.steps[i]);
		if(dir == last) {
			++count;
		} else {
			if(count > 0) {
				legs.push_back(std::to_string(count) + " " + direction_name(last));
			}
			last = dir;
			count = 1;
		}
	}
	if(count > 0) {
		legs.push_back(std::to_string(count) + " " + direction_name(last));
	}
	if(prefs().verbosity >= 1) {
		parts.push_back(_("path") + std::string(": ") + join(legs));
	}
	speak(join(parts, ". "), msg::reply);
}

void describe_unit(bool full)
{
	const unit* u = visible_unit(g.cursor);
	if(!u) {
		speak(loc_shrouded(g.cursor) ? _("Unexplored.") : _("No visible unit here."), msg::reply);
		return;
	}
	sync_hover(g.cursor);
	if(disp().displayed_unit_hex() != g.cursor) {
		// Fall back to direct information if the display shows another unit.
		speak(unit_brief(*u), msg::reply);
		return;
	}
	std::vector<std::string> parts;
	parts.push_back(unit_title(*u));
	parts.push_back(side_label(u->side()));
	parts.push_back(_("level") + std::string(" ") + std::to_string(u->level()));
	parts.push_back(hp_text(*u));
	parts.push_back(_("experience") + std::string(" ") + std::to_string(u->experience()) + " " + _("of") + " " + std::to_string(u->max_experience()));
	parts.push_back(std::to_string(u->movement_left()) + " " + _("of") + " " + std::to_string(u->total_movement()) + " " + _("moves"));
	parts.push_back(std::to_string(u->attacks_left()) + " " + _("of") + " " + std::to_string(u->max_attacks()) + " " + _("attacks left"));
	auto st = status_list(*u);
	parts.insert(parts.end(), st.begin(), st.end());
	report_result align = report("unit_alignment");
	parts.push_back(_("alignment") + std::string(" ") + align.text);
	report_result traits = report("unit_traits");
	if(!traits.text.empty()) {
		parts.push_back(_("traits") + std::string(": ") + traits.text);
	}
	report_result abil = report("unit_abilities");
	if(!abil.text.empty()) {
		parts.push_back(_("abilities") + std::string(": ") + abil.text);
	}
	const t_translation::terrain_code terrain = gmap().get_terrain(u->get_location());
	parts.push_back(std::to_string(100 - u->defense_modifier(terrain)) + "% " + _("defense on") + " " + terrain_name(u->get_location()));
	// Weapons with the effective (time of day, leadership...) values of the native sidebar.
	report_result weapons = report("unit_weapons");
	parts.push_back(_("attacks") + std::string(": ") + weapons.text);
	if(full) {
		if(!align.tooltip.empty()) parts.push_back(align.tooltip);
		if(!traits.tooltip.empty()) parts.push_back(traits.tooltip);
		if(!abil.tooltip.empty()) parts.push_back(abil.tooltip);
		if(!weapons.tooltip.empty()) parts.push_back(weapons.tooltip);
		report_result hp = report("unit_hp");
		if(!hp.tooltip.empty()) parts.push_back(hp.tooltip); // resistances
		report_result adv = report("unit_advancement_options");
		if(!adv.text.empty()) parts.push_back(_("advances to") + std::string(": ") + adv.text);
		report_result moves = report("unit_moves");
		if(!moves.tooltip.empty()) parts.push_back(moves.tooltip); // movement costs
		report_result def = report("unit_defense");
		if(!def.tooltip.empty()) parts.push_back(def.tooltip);
		report_result vision = report("unit_vision");
		if(!vision.tooltip.empty()) parts.push_back(vision.tooltip);
	}
	parts.push_back(coords(u->get_location()));
	speak(join(parts, ". "), msg::reply);
}

void speak_objectives()
{
	std::string obj = plain_text(viewer().objectives().str());
	if(obj.empty()) {
		speak(_("No objectives are defined."), msg::reply);
		return;
	}
	speak(_("Objectives") + std::string(". ") + obj, msg::reply);
}

std::string economy_text()
{
	std::vector<std::string> parts;
	report_result turn = report("turn");
	std::string t = turn.text;
	auto slash = t.find('/');
	if(slash != std::string::npos) {
		t = t.substr(0, slash) + " " + _("of") + " " + t.substr(slash + 1);
	}
	parts.push_back(_("Turn") + std::string(" ") + t);
	parts.push_back(side_label(viewing_side()));
	parts.push_back(_("gold") + std::string(" ") + report("gold").text);
	parts.push_back(_("income") + std::string(" ") + report("income").text);
	std::string upkeep = report("upkeep").text;
	parts.push_back(_("upkeep") + std::string(" ") + upkeep);
	std::string villages = report("villages").text;
	slash = villages.find('/');
	if(slash != std::string::npos) {
		villages = villages.substr(0, slash) + " " + _("of") + " " + villages.substr(slash + 1);
	}
	parts.push_back(_("villages") + std::string(" ") + villages);
	return join(parts);
}

std::string tod_text()
{
	report_result tod = report("time_of_day");
	std::string res = tod.tooltip.empty() ? tod.text : tod.tooltip;
	return res;
}

std::string counts_text()
{
	int mine = static_cast<int>(build_list(L_UNITS).size());
	int act = static_cast<int>(build_list(L_ACTIVE).size());
	int wounded = static_cast<int>(build_list(L_WOUNDED).size());
	int enemies = static_cast<int>(build_list(L_ENEMIES).size());
	int allies = static_cast<int>(build_list(L_ALLIES).size());
	std::vector<std::string> parts {
		std::to_string(mine) + " " + _("units"),
		std::to_string(act) + " " + _("can still act"),
		std::to_string(wounded) + " " + _("wounded"),
		std::to_string(enemies) + " " + _("visible enemies"),
	};
	if(allies > 0) {
		parts.push_back(std::to_string(allies) + " " + _("allied units"));
	}
	return join(parts);
}

void goto_leader()
{
	for(const unit& u : board().units()) {
		if(u.side() == viewing_side() && u.can_recruit()) {
			set_cursor(u.get_location(), true);
			return;
		}
	}
	speak(_("You have no leader."), msg::reply);
}

void goto_leader();

void goto_selected()
{
	map_location sel = mouse().get_selected_hex();
	if(!sel.valid() || !visible_unit(sel)) {
		// Nothing selected: go to the leader instead.
		goto_leader();
		return;
	}
	set_cursor(sel, true);
}

bool is_printable(const SDL_Event& event)
{
	const SDL_Keycode k = event.key.keysym.sym;
	const unsigned mod = event.key.keysym.mod;
	return !(mod & (KMOD_CTRL | KMOD_ALT)) && k >= 32 && k < 127;
}

bool execute(command cmd)
{
	switch(cmd) {
	case command::move_n: case command::move_ne: case command::move_se:
	case command::move_s: case command::move_sw: case command::move_nw:
		move_cursor(dir_of(cmd), false);
		return true;
	case command::jump_n: case command::jump_ne: case command::jump_se:
	case command::jump_s: case command::jump_sw: case command::jump_nw:
		move_cursor(dir_of(cmd), true);
		return true;
	case command::select_action: select_or_act(); return true;
	case command::describe_tile: speak(describe_tile(g.cursor, 2, true), msg::reply); return true;
	case command::describe_route: describe_route(); return true;
	case command::describe_unit: describe_unit(false); return true;
	case command::describe_unit_full: describe_unit(true); return true;
	case command::where_am_i: where_am_i(); return true;
	case command::map_info: map_info(); return true;
	case command::adjacent: describe_adjacent(); return true;
	case command::goto_selected: goto_selected(); return true;
	case command::goto_leader: goto_leader(); return true;
	case command::goto_previous:
		if(g.previous.valid() && on_board(g.previous)) {
			set_cursor(g.previous, true);
		} else {
			speak(_("No previous location."), msg::reply);
		}
		return true;
	case command::goto_coordinates: goto_coordinates(); return true;
	case command::next_unit: cycle_list(L_UNITS, 1); return true;
	case command::prev_unit: cycle_list(L_UNITS, -1); return true;
	case command::next_active_unit: cycle_list(L_ACTIVE, 1); return true;
	case command::prev_active_unit: cycle_list(L_ACTIVE, -1); return true;
	case command::next_wounded: cycle_list(L_WOUNDED, 1); return true;
	case command::prev_wounded: cycle_list(L_WOUNDED, -1); return true;
	case command::next_ally: cycle_list(L_ALLIES, 1); return true;
	case command::prev_ally: cycle_list(L_ALLIES, -1); return true;
	case command::next_enemy: cycle_list(L_ENEMIES, 1); return true;
	case command::prev_enemy: cycle_list(L_ENEMIES, -1); return true;
	case command::next_village: cycle_list(L_VILLAGES, 1); return true;
	case command::prev_village: cycle_list(L_VILLAGES, -1); return true;
	case command::next_label: cycle_list(L_LABELS, 1); return true;
	case command::prev_label: cycle_list(L_LABELS, -1); return true;
	case command::next_reachable: cycle_list(L_REACH, 1); return true;
	case command::prev_reachable: cycle_list(L_REACH, -1); return true;
	case command::next_target: cycle_list(L_TARGETS, 1); return true;
	case command::prev_target: cycle_list(L_TARGETS, -1); return true;
	case command::objectives: speak_objectives(); return true;
	case command::economy: speak(economy_text(), msg::reply); return true;
	case command::time_of_day: speak(tod_text(), msg::reply); return true;
	case command::unit_counts: speak(counts_text(), msg::reply); return true;
	case command::native_describe_unit:
		sync_hover(g.cursor);
		g.pc->get_menu_handler().unit_description();
		return true;
	case command::native_statistics:
		g.pc->get_menu_handler().show_statistics(viewing_side());
		return true;
	default:
		break;
	}
	int b = static_cast<int>(cmd) - static_cast<int>(command::set_bookmark_1);
	if(b >= 0 && b < 9) {
		g.bookmarks[b + 1] = g.cursor;
		speak(_("Bookmark") + std::string(" ") + std::to_string(b + 1) + " " + _("set at") + " " + coords(g.cursor), msg::reply);
		return true;
	}
	b = static_cast<int>(cmd) - static_cast<int>(command::goto_bookmark_1);
	if(b >= 0 && b < 9) {
		if(g.bookmarks[b + 1].valid() && on_board(g.bookmarks[b + 1])) {
			set_cursor(g.bookmarks[b + 1], true);
		} else {
			speak(_("Bookmark") + std::string(" ") + std::to_string(b + 1) + " " + _("is not set"), msg::reply);
		}
		return true;
	}
	return false;
}

} // anonymous namespace

// --------------------------------------------------------------- public ---

std::string direction_name(map_location::DIRECTION dir)
{
	switch(dir) {
	case map_location::NORTH: return _("north");
	case map_location::NORTH_EAST: return _("north-east");
	case map_location::SOUTH_EAST: return _("south-east");
	case map_location::SOUTH: return _("south");
	case map_location::SOUTH_WEST: return _("south-west");
	case map_location::NORTH_WEST: return _("north-west");
	default: return std::string();
	}
}

std::string relative_position(const map_location& from, const map_location& to)
{
	if(from == to) {
		return _("same hex");
	}
	int d = static_cast<int>(distance_between(from, to));
	return distance_text(d) + " " + direction_name(from.get_relative_dir(to));
}

bool handle_event(controller_base& controller, const SDL_Event& event)
{
	if(event.type == SDL_TEXTINPUT) {
		if(g.suppress_text_input) {
			g.suppress_text_input = false;
			return true;
		}
		return false;
	}
	if(event.type != SDL_KEYDOWN) {
		return false;
	}
	g.suppress_text_input = false;

	if(handle_global_key(event)) {
		g.suppress_text_input = is_printable(event);
		return true;
	}
	auto* pc = dynamic_cast<play_controller*>(&controller);
	if(!enabled() || !pc || pc != g.pc) {
		return false;
	}

	command cmd = keymap::lookup(context::map, event);
	if(cmd == command::none) {
		return false;
	}
	if(cmd == command::cancel) {
		// Escape: deselect first. With nothing selected the game asks whether
		// to quit; require a second Escape so it is not opened by accident.
		if(mouse().get_selected_hex().valid()) {
			cancel_selection();
			g.last_escape = 0;
			return true;
		}
		const uint32_t now = SDL_GetTicks();
		if(g.last_escape != 0 && now - g.last_escape < 3000) {
			g.last_escape = 0;
			return false;
		}
		g.last_escape = now;
		speak(_("Nothing selected. Press Escape again to open the quit question."), msg::reply);
		return true;
	}
	if(!on_board(g.cursor)) {
		// Start at the selected unit or the leader.
		map_location sel = mouse().get_selected_hex();
		if(on_board(sel)) {
			g.cursor = sel;
		} else {
			for(const unit& u : board().units()) {
				if(u.side() == viewing_side() && u.can_recruit()) {
					g.cursor = u.get_location();
					break;
				}
			}
		}
		if(!on_board(g.cursor)) {
			g.cursor = map_location(0, 0);
		}
	}
	try {
		bool done = execute(cmd);
		if(done) {
			g.suppress_text_input = is_printable(event);
		}
		return done;
	} catch(const std::exception& e) {
		log(std::string("map command failed: ") + keymap::command_id(cmd) + ": " + e.what());
		return true;
	}
}

void on_focus_gained(controller_base& controller)
{
	auto* pc = dynamic_cast<play_controller*>(&controller);
	if(!enabled() || !pc || pc != g.pc || gui::dialog_active()) {
		return;
	}
	speak(_("Game map") + std::string(". ") + describe_tile(g.cursor, 0, true), msg::reply);
}

void on_controller_start(play_controller& pc)
{
	g = game_state_access();
	g.pc = &pc;
	log("game started: " + plain_text(pc.get_scenario_name().str()));
}

void on_controller_end(play_controller& pc)
{
	if(g.pc == &pc) {
		g.pc = nullptr;
		log("game controller ended");
	}
}

void on_human_turn_start(play_controller& pc)
{
	if(!enabled() || g.pc != &pc) {
		return;
	}
	// Put the cursor on the leader (or first unit) at the start of the turn.
	if(!on_board(g.cursor)) {
		for(const unit& u : board().units()) {
			if(u.side() == viewing_side() && u.can_recruit()) {
				g.cursor = u.get_location();
				break;
			}
		}
	}
	cue("turn");
	std::vector<std::string> parts;
	parts.push_back(_("Your turn"));
	// Healing and poison happened before the turn began.
	std::vector<std::string> heals;
	heals.swap(g.heal_lines);
	if(prefs().turn_summary) {
		parts.push_back(economy_text());
		parts.push_back(tod_text());
		parts.push_back(counts_text());
	} else {
		parts.push_back(_("Turn") + std::string(" ") + report("turn").text);
	}
	parts.insert(parts.end(), heals.begin(), heals.end());
	speak(join(parts, ". "), msg::important);
	log("human turn start: side " + std::to_string(pc.current_side()));
}

void on_other_turn_start(play_controller& pc, int side)
{
	if(!enabled() || g.pc != &pc) {
		return;
	}
	g.other_turn_log.clear();
	g.other_turn_side = side;
	std::vector<std::string> heals;
	heals.swap(g.heal_lines);
	if(prefs().enemy_verbosity >= 1) {
		std::string who = viewer().is_enemy(side) ? _("Enemy turn") : _("Allied turn");
		std::vector<std::string> parts {who + ": " + side_label(side)};
		if(prefs().enemy_verbosity >= 2) {
			parts.insert(parts.end(), heals.begin(), heals.end());
		} else {
			g.other_turn_log.insert(g.other_turn_log.end(), heals.begin(), heals.end());
		}
		speak(join(parts, ". "), msg::event);
	}
}

void on_other_turn_end(play_controller& pc, int side)
{
	if(!enabled() || g.pc != &pc) {
		return;
	}
	if(prefs().enemy_verbosity == 1) {
		if(g.other_turn_log.empty()) {
			speak(side_label(side) + ": " + _("no visible actions"), msg::event);
		} else {
			speak(side_label(side) + ": " + join(g.other_turn_log, ". "), msg::event);
		}
	}
	g.other_turn_log.clear();
	g.other_turn_side = 0;
}

void on_linger(play_controller& pc)
{
	if(!enabled() || g.pc != &pc || !pc.is_linger_mode()) {
		return;
	}
	speak(_("The scenario is over. You can still review the map. Press Control Space to end the scenario."), msg::important);
}

namespace
{
/** Routes an event message according to whose turn it is. */
void event_message(const std::string& text, bool important = false)
{
	if(text.empty()) {
		return;
	}
	const bool other_turn = g.other_turn_side != 0;
	if(other_turn) {
		if(prefs().enemy_verbosity == 0) {
			return;
		}
		if(prefs().enemy_verbosity == 1) {
			g.other_turn_log.push_back(text);
			return;
		}
	}
	speak(text, important ? msg::important : msg::event);
}
} // namespace

void on_select(const map_location& hex)
{
	if(!active()) {
		return;
	}
	if(!hex.valid()) {
		g.last_selected = map_location();
		g.last_selected_id.clear();
		return;
	}
	const unit* u = visible_unit(hex);
	if(!u) {
		return;
	}
	// The engine re-selects the same unit internally (e.g. after a path
	// refresh); only announce real selection changes.
	if(g.last_selected == hex && g.last_selected_id == u->id()) {
		return;
	}
	g.last_selected = hex;
	g.last_selected_id = u->id();
	g.cursor = hex;
	std::vector<std::string> parts {_("Selected") + std::string(" ") + unit_brief(*u)};
	if(u->side() == viewing_side()) {
		const pathfind::paths& paths = mouse().current_paths();
		int reach = 0;
		for(const auto& d : paths.destinations) {
			if(d.curr != hex && !visible_unit(d.curr)) {
				++reach;
			}
		}
		parts.push_back(std::to_string(reach) + " " + _("reachable hexes"));
		int targets = 0;
		std::set<map_location> seen;
		for(const auto& e : build_list(L_TARGETS)) {
			if(seen.insert(e.loc).second) {
				++targets;
			}
		}
		if(targets > 0) {
			parts.push_back(std::to_string(targets) + " " + _("enemies in attack range"));
		}
	}
	cue("select");
	speak(join(parts, ". "), msg::reply);
}

void on_move(const unit* u, const map_location& from, const map_location& to, int side, bool captured_village)
{
	if(!active() || from == to) {
		return;
	}
	// Visible start: the hex is not fogged and the unit was not hidden there
	// (ambush, nightstalk...). A sighted player sees nothing else.
	const bool from_visible = on_board(from) && !loc_fogged(from)
		&& (!u || side == viewing_side() || !u->invisible(from));
	const bool to_visible = u && visible_unit(to) == u;
	if(!from_visible && !to_visible) {
		return; // nothing a sighted player could see
	}
	std::string who = u && to_visible ? unit_title(*u) : _("a unit");
	std::string text;
	if(side == viewing_side() && u) {
		text = who + " " + _("moved to") + " " + coords(to) + ", " + terrain_name(to);
		if(u->movement_left() > 0) {
			text += ", " + std::to_string(u->movement_left()) + " " + _("moves left");
		}
		g.cursor = to;
	} else if(to_visible) {
		text = who + " " + _("moves to") + " " + coords(to) + ", " + terrain_name(to);
	} else {
		text = who + " " + _("moves out of sight from") + " " + coords(from);
	}
	if(captured_village && to_visible) {
		text += ", " + std::string(_("captures the village"));
	}
	event_message(text);
}

void on_attack_start(const unit& attacker, const unit& defender, const battle_context_unit_stats& a, const battle_context_unit_stats& d)
{
	if(!active()) {
		return;
	}
	g.in_combat = true;
	g.combat_lines.clear();
	g.combat_visible = !loc_fogged(attacker.get_location()) || !loc_fogged(defender.get_location());
	if(!g.combat_visible) {
		return;
	}
	std::string aw = a.weapon ? plain_text(a.weapon->name()) : std::string();
	std::string dw = d.weapon ? plain_text(d.weapon->name()) : std::string(_("no weapon"));
	std::string text = unit_title(attacker) + " " + _("attacks") + " " + unit_title(defender) + " " + _("with") + " " + aw + ", "
		+ std::to_string(a.damage) + "-" + std::to_string(a.num_blows) + ", " + std::to_string(a.chance_to_hit) + "% " + _("to hit")
		+ ". " + _("Retaliation") + ": " + dw;
	if(d.weapon) {
		text += ", " + std::to_string(d.damage) + "-" + std::to_string(d.num_blows) + ", " + std::to_string(d.chance_to_hit) + "% " + _("to hit");
	}
	event_message(text);
}

void on_strike(const unit& attacker, const unit& defender, const std::string& weapon, bool hits, int damage,
	bool dies, bool poisons, bool slows, bool petrifies, int drained)
{
	if(!active() || !g.combat_visible) {
		return;
	}
	(void)weapon;
	std::string text;
	if(hits) {
		text = attacker.type_name().str() + " " + _("hits") + ", " + std::to_string(damage) + " " + _("damage");
		std::vector<std::string> fx;
		if(poisons) fx.push_back(_("poisoned"));
		if(slows) fx.push_back(_("slowed"));
		if(petrifies) fx.push_back(_("petrified"));
		if(drained > 0) fx.push_back(_("drains") + std::string(" ") + std::to_string(drained));
		if(!fx.empty()) {
			text += ", " + join(fx);
		}
		if(dies) {
			text += ". " + unit_title(defender) + " " + _("dies");
		} else {
			text += ", " + defender.type_name().str() + " " + std::to_string(defender.hitpoints()) + " " + _("left");
		}
	} else {
		text = attacker.type_name().str() + " " + _("misses");
	}
	if(prefs().combat_verbosity >= 1) {
		event_message(text, dies);
	} else if(dies) {
		g.combat_lines.push_back(unit_title(defender) + " " + _("dies"));
	}
}

void on_attack_end(const unit* attacker, const unit* defender, int attacker_xp, int defender_xp)
{
	if(!active()) {
		return;
	}
	g.in_combat = false;
	if(!g.combat_visible) {
		return;
	}
	std::vector<std::string> parts = g.combat_lines;
	g.combat_lines.clear();
	if(prefs().combat_verbosity == 0) {
		if(attacker) parts.push_back(unit_title(*attacker) + " " + hp_text(*attacker));
		if(defender) parts.push_back(unit_title(*defender) + " " + hp_text(*defender));
	}
	if(attacker && attacker->side() == viewing_side() && attacker_xp > 0) {
		parts.push_back(attacker->type_name().str() + " " + _("gains") + " " + std::to_string(attacker_xp) + " " + _("experience"));
	}
	if(defender && defender->side() == viewing_side() && defender_xp > 0) {
		parts.push_back(defender->type_name().str() + " " + _("gains") + " " + std::to_string(defender_xp) + " " + _("experience"));
	}
	event_message(join(parts, ". "));
}

void on_unit_placed(const unit& u, bool recall)
{
	if(!active()) {
		return;
	}
	const unit* vis = visible_unit(u.get_location());
	if(vis != &u) {
		return;
	}
	std::string text = unit_title(u) + " " + (recall ? _("recalled at") : _("recruited at")) + " " + coords(u.get_location());
	if(u.side() == viewing_side()) {
		text += ". " + std::string(_("gold")) + " " + std::to_string(board().get_team(u.side()).gold());
		g.cursor = u.get_location();
	}
	event_message(text);
}

void on_undo_redo(bool done, bool redo)
{
	if(!active() || g.other_turn_side != 0) {
		return;
	}
	if(!done) {
		speak(redo ? _("Nothing to redo.") : _("Nothing to undo."), msg::reply);
		cue("error");
		return;
	}
	// The undone action is reverted silently by the engine; describe the cursor
	// tile so the result can be checked.
	std::string text = _("Undone");
	if(const unit* u = visible_unit(g.cursor)) {
		text += ". " + unit_brief(*u) + ", " + coords(g.cursor);
	}
	speak(text, msg::reply);
}

void on_heal(const unit& u, int amount, bool cured)
{
	if(!active() || visible_unit(u.get_location()) != &u) {
		return;
	}
	std::vector<std::string> parts {unit_title(u)};
	if(cured) {
		parts.push_back(_("cured of poison"));
	}
	if(amount > 0) {
		parts.push_back(_("heals") + std::string(" ") + std::to_string(amount));
	} else if(amount < 0) {
		parts.push_back(_("loses") + std::string(" ") + std::to_string(-amount) + " " + _("hit points to poison"));
	}
	if(amount != 0) {
		parts.push_back(hp_text(u));
	}
	g.heal_lines.push_back(join(parts));
}

void on_advance(const unit& u)
{
	if(!active()) {
		return;
	}
	if(visible_unit(u.get_location()) != &u) {
		return;
	}
	event_message(unit_title(u) + " " + _("advanced to") + " " + u.type_name().str(), true);
}

void on_announce(const std::string& text)
{
	if(!enabled()) {
		return;
	}
	event_message(plain_text(text), true);
}

void on_overlay_text(const std::string& text)
{
	if(!enabled()) {
		return;
	}
	std::string t = plain_text(text);
	if(t.empty()) {
		return;
	}
	// Overlays carry instructions (tutorial hints, scenario notes): keep them
	// in the dialogue history too, so F11 can repeat them.
	add_dialogue("", t);
	speak(t, msg::important);
}

void on_chat(const std::string& speaker, const std::string& text)
{
	if(!enabled()) {
		return;
	}
	std::string s = plain_text(speaker);
	std::string t = plain_text(text);
	speak(s.empty() ? t : s + ": " + t, msg::event);
	add_dialogue(s, t);
}

} // namespace accessibility::game
