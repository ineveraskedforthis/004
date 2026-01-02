#include <cmath>

#define PI 3.14159265358979323846

#ifndef floorf
namespace std {
	float floorf(float x) { return ::floor(x); }
	float ceilf(float x) { return ::ceil(x); }
}
#endif

#include "iostream"
#include "data.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <signal.h>
#include <chrono>

#include "network_containers.hpp"
#include "unordered_dense.h"

constexpr float PARRY_PREPARATION = 0.3f;
constexpr float PARRY_DURATION = 0.3f;

constexpr float INVISIBILITY_PREPARATION = 2.f;
constexpr float INVISIBILITY_DURATION = 13.f;

constexpr float SPELL_PREPARATION = 1.f;

constexpr float CHARGE_PREPARATION = 2.f;

constexpr float ATTACK_PREPARATION = 0.5f;

constexpr float ATTACK_RANGE = 0.2f;

constexpr float MEDIUM_STUN_DURATION = 1.f;
constexpr float SHORT_STUN_DURATION = 0.1f;

struct game_session {
        dcon::data_container state{};
};

void event_notification(game_session& game, dcon::fighter_id fid, uint8_t event_type) {
	game.state.for_each_player([&](auto dest) {
		update::data to_send {};
		to_send.id = fid.index();
		to_send.update_type = update::EVENT;
		to_send.event_type = event_type;
		auto connection = game.state.player_get_connection(dest);
		if (connection)
			send(connection, (char*)&to_send, sizeof(update::data), 0);
	});
}

void event_notification(game_session& game, dcon::fighter_id fid, uint8_t event_type, float x, float y) {
	printf("send event %d\n", event_type);
	game.state.for_each_player([&](auto dest) {
		update::data to_send {};
		to_send.id = fid.index();
		to_send.update_type = update::EVENT;
		to_send.event_type = event_type;
		to_send.x = x;
		to_send.y = y;
		auto connection = game.state.player_get_connection(dest);
		send(connection, (char*)&to_send, sizeof(update::data), 0);
	});
}

void event_notification_to_player(game_session& game, dcon::player_id pid, uint8_t event_type) {
	update::data to_send {};
	to_send.id = pid.index();
	to_send.update_type = update::EVENT;
	to_send.event_type = event_type;

	auto connection = game.state.player_get_connection(pid);

	send(connection, (char*) & to_send, sizeof(update::data), 0);
}

bool is_busy(game_session& game, dcon::fighter_id fid) {
	if (game.state.fighter_get_stunned_timer(fid) > 0.f) return true;
	if (game.state.fighter_get_charge_timer(fid) > 0.f) return true;
	if (game.state.fighter_get_action_timer(fid) > 0.f) return true;

	return false;
}

float move_speed_from_wrong_direction(
	game_session& game, 
	dcon::fighter_id fid,
	float dx, 
	float dy
) {
	if (dx == 0.f && dy== 0.f) {
		return 1.f;
	}
	auto desired_direction = atan2f(dy, dx);
	auto direction = game.state.fighter_get_direction(fid);
	return 0.1 + std::max(0.f, cosf(direction - desired_direction));
}

void rotate_toward(game_session& game, float dt, dcon::fighter_id fid, float dx, float dy, float rotation_speed) {
	if (dx != 0 || dy != 0) {
		auto desired_direction = atan2f(dy, dx);
		auto direction = game.state.fighter_get_direction(fid);
		auto diff = fmodf(desired_direction - direction + 4 * PI, 2 * PI);
		if (diff <= rotation_speed * dt) {
			direction = desired_direction;
		} else if (diff <= PI) {
			direction = direction + rotation_speed * dt;
		} else if (diff < 2 * PI - rotation_speed * dt) {
			direction = direction - rotation_speed * dt;
		} else {
			direction = desired_direction;
		}
		direction = fmodf(direction + 2 * PI, 2 * PI);
		game.state.fighter_set_direction(fid, direction);
	}
}

void stun(game_session& game, dcon::fighter_id fid, float duration) {
	game.state.fighter_set_action_timer(fid, 0.f);
	game.state.fighter_set_charge_timer(fid, 0.f);
	game.state.fighter_set_action_timer(fid, 0.f);

	game.state.fighter_set_stunned_timer(fid, duration);

	event_notification(game, fid, update::EVENT_STUN, duration, 0.f);
}

float distance(game_session& game, dcon::fighter_id a, dcon::fighter_id b) {
	auto xa = game.state.fighter_get_x(a);
	auto ya = game.state.fighter_get_y(a);
	auto xb = game.state.fighter_get_x(b);
	auto yb = game.state.fighter_get_y(b);

	auto dx = xb - xa;
	auto dy = yb - ya;

	return sqrtf(dx * dx + dy * dy);
}

bool can_be_selected(game_session& game, dcon::fighter_id origin, dcon::fighter_id candidate) {
	if (!candidate) return false;
	if (!game.state.fighter_is_valid(candidate)) return false;
	if (game.state.fighter_get_invisible_timer(candidate) > 0.f) return false;
	if (game.state.fighter_get_hp(candidate) <= 0) return false;
	if (game.state.fighter_get_hp(origin) <= 0) return false;

	auto control = game.state.fighter_get_player_control(origin);
	auto player = game.state.player_control_get_controller(control);
	auto location = game.state.player_get_location(player);
	auto room = game.state.location_get_where(location);

	auto tcontrol = game.state.fighter_get_player_control(candidate);
	auto tplayer = game.state.player_control_get_controller(tcontrol);
	auto tlocation = game.state.player_get_location(tplayer);
	auto troom = game.state.location_get_where(tlocation);

	if (troom != room) return false;

	return true;
}

void shoot_spell(game_session& game, dcon::fighter_id origin, dcon::fighter_id target) {
	if (!can_be_selected(game, origin, target)) return;

	auto x = game.state.fighter_get_x(origin);
	auto y = game.state.fighter_get_y(origin);

	auto control = game.state.fighter_get_player_control(origin);
	auto player = game.state.player_control_get_controller(control);
	auto location = game.state.player_get_location(player);
	auto room = game.state.location_get_where(location);

	auto proj = game.state.create_projectile();
	game.state.projectile_set_x(proj, x);
	game.state.projectile_set_y(proj, y);
	game.state.force_create_homing_target(target, proj);
	game.state.force_create_projectile_location(proj, room);
	printf("new proj\n");
}

void deal_damage(game_session& game, dcon::fighter_id origin, dcon::fighter_id target, int damage) {
	game.state.fighter_set_hp(target,
		std::max(0, game.state.fighter_get_hp(target) - damage)
	);
	game.state.fighter_set_invisible_timer(origin, 0.f);
}

void melee_arc_damage(game_session& game, dcon::fighter_id origin) {
	auto damage = 1;
	if (game.state.fighter_get_invisible_timer(origin) > 0.f) {
		damage *= 2;
	}

	auto x = game.state.fighter_get_x(origin);
	auto y = game.state.fighter_get_y(origin);

	game.state.for_each_fighter([&](auto target){
		if (!can_be_selected(game, origin, target)) {
			return;
		}

		if (target == origin) {
			return;
		}

		auto tx = game.state.fighter_get_x(target);
		auto ty = game.state.fighter_get_y(target);

		auto dx = tx - x;
		auto dy = ty - y;

		auto norm = sqrt(dx * dx + dy * dy);

		auto dir = atan2f(dy, dx);

		auto dif = fmodf(abs(dir - game.state.fighter_get_direction(origin)), 2 * PI);

		bool good_direction = false;
		if (dif < 1 || dif > 2 * PI - 1) {
			good_direction = true;
		}

		if (norm > ATTACK_RANGE || (dx == 0.f && dy == 0.f)) {
			good_direction = false;
		}

		if (norm < ATTACK_RANGE * 0.1f || good_direction) {
			if (game.state.fighter_get_no_damage_timer(target) > 0.f) {
				stun(game, origin, MEDIUM_STUN_DURATION);
			} else {
				deal_damage(game, origin, target, damage);
			}
		}
	});
}

void execute_jump_behind(game_session& game, float dt, bool completed, dcon::fighter_id origin) {
auto selection = game.state.fighter_get_selection_as_selector(origin);
	auto selected = game.state.selection_get_selected(selection);

	if (selected) {
		float x = game.state.fighter_get_x(origin);
		float y = game.state.fighter_get_y(origin);

		float target_direction = game.state.fighter_get_direction(selected);
		float tx = game.state.fighter_get_x(selected) - cosf(target_direction) * ATTACK_RANGE * 0.5f;
		float ty = game.state.fighter_get_y(selected) - sinf(target_direction) * ATTACK_RANGE * 0.5f;
		
		float dx = tx - x;
		float dy = ty - y;

		float n = sqrtf(dx * dx + dy * dy);

		if (n < ATTACK_RANGE) {
			game.state.fighter_set_x(origin, tx);
			game.state.fighter_set_y(origin, ty);
			completed = true;
		} else {					
			game.state.fighter_set_x(origin, x + dx / n * dt);
			game.state.fighter_set_y(origin, y + dy / n  * dt);
		}

		if (completed) {
				
			if (selected) {
				x = game.state.fighter_get_x(origin);
				y = game.state.fighter_get_y(origin);
				float tx = game.state.fighter_get_x(selected);
				float ty = game.state.fighter_get_y(selected);
				float dx = tx - x;
				float dy = ty - y;

				if (dx != 0 || dy != 0) {
					float dir = atan2f(dy, dx);
					game.state.fighter_set_direction(origin, fmodf(dir + 2.f * PI, 2 * PI));
				}
			}
			game.state.fighter_set_action_type(origin, command::MOVE);
			game.state.fighter_set_action_timer(origin, 0.f);
		} 
	}
}

void execute_attack(game_session& game, float dt, bool completed, dcon::fighter_id origin) {
	// move closer to target if it's far away
	// if there is no target, we move toward current direction
	// at the end we deal damage to fighters in the area

	auto selection = game.state.fighter_get_selection_as_selector(origin);
	auto selected = game.state.selection_get_selected(selection);

	if (selected) {
		if (completed) {
			melee_arc_damage(game, origin);
			game.state.fighter_set_action_type(origin, command::MOVE);
			game.state.fighter_set_action_timer(origin, 0.f);
		} else {
			float x = game.state.fighter_get_x(origin);
			float y = game.state.fighter_get_y(origin);
			
			float dx = game.state.fighter_get_x(selected) - x;
			float dy = game.state.fighter_get_y(selected) - y;

			rotate_toward(game, dt, origin, dx, dy, 3.f);
			float speed_mod = move_speed_from_wrong_direction(game, origin, dx, dy);
			
			float n = sqrtf(dx * dx + dy * dy);

			if (n > ATTACK_RANGE * 0.5) {
				game.state.fighter_set_x(origin, x + dx / n * 0.9 * dt * speed_mod);
				game.state.fighter_set_y(origin, y + dy / n * 0.9 * dt * speed_mod);
			}
		}
	} else {
		if (completed) {
			melee_arc_damage(game, origin);
			game.state.fighter_set_action_type(origin, command::MOVE);
			game.state.fighter_set_action_timer(origin, 0.f);
		} else {
			float direction = game.state.fighter_get_direction(origin);
			float dx = cosf(direction);
			float dy = sinf(direction);

			float x = game.state.fighter_get_x(origin);
			float y = game.state.fighter_get_y(origin);

			game.state.fighter_set_x(origin, x + dx * 0.9 * dt);
			game.state.fighter_set_y(origin, y + dy * 0.9 * dt);
		}
	}
}

void execute_charge(game_session& game, float dt, bool completed, dcon::fighter_id origin) {
	auto selection = game.state.fighter_get_selection_as_selector(origin);
	auto selected = game.state.selection_get_selected(selection);

	if (selected) {
		float x = game.state.fighter_get_x(origin);
		float y = game.state.fighter_get_y(origin);
		
		float dx = game.state.fighter_get_x(selected) - x;
		float dy = game.state.fighter_get_y(selected) - y;

		float n = sqrtf(dx * dx + dy * dy);

		if (n < ATTACK_RANGE) {
			completed = true;
		} else {
			
			rotate_toward(game, dt, origin, dx, dy, 3.f);
			float speed_mod = move_speed_from_wrong_direction(game, origin, dx, dy);

			game.state.fighter_set_x(origin, x + dx / n * 2 * dt * speed_mod);
			game.state.fighter_set_y(origin, y + dy / n * 2 * dt * speed_mod);
		}

		if (completed && n < ATTACK_RANGE) {
			stun(game, selected, MEDIUM_STUN_DURATION);
			game.state.fighter_set_action_type(origin, command::MOVE);
			game.state.fighter_set_action_timer(origin, 0.f);
		} 
	}
}

void update_game_state(game_session& game, std::chrono::microseconds last_tick) {
	float dt = float(last_tick.count()) / 1'000'000.f;

	game.state.for_each_fighter([&](auto fid){
		auto x = game.state.fighter_get_x(fid);
		auto y = game.state.fighter_get_y(fid);
		auto tx = game.state.fighter_get_tx(fid);
		auto ty = game.state.fighter_get_ty(fid);
		auto dx = tx;
		auto dy = ty;

		auto rotation_speed = 4.5f;
		float speed_mod = 0.7f * move_speed_from_wrong_direction(game, fid, dx, dy);

		rotate_toward(game, dt, fid, dx, dy, rotation_speed);

		auto stunned = game.state.fighter_get_stunned_timer(fid);	
		float progress_mod = 1.f;

		if (stunned > 0.f) {
			game.state.fighter_set_stunned_timer(fid, std::max(0.f, stunned - dt));
			speed_mod = 0.f;
			progress_mod = 0.f;
			rotation_speed = 0.f;
		}

		auto norm = sqrtf(dx * dx + dy * dy);
		if (norm > 1.f) {
			dx /= norm;
			dy /= norm;
		}

		float progress = game.state.fighter_get_action_timer(fid);

		auto control = game.state.fighter_get_player_control(fid);
		auto player = game.state.player_control_get_controller(control);
		auto location = game.state.player_get_location(player);
		auto room = game.state.location_get_where(location);
		auto selection = game.state.fighter_get_selection_as_selector(fid);
		auto selected = game.state.selection_get_selected(selection);

		if (
			!can_be_selected(game, fid, selected)
		) {
			selected = dcon::fighter_id {};
			game.state.delete_selection(selection);
			selection = dcon::selection_id {};
		}

		
		float edt = progress_mod * dt;

		if (progress > 0.f) {
			auto action = game.state.fighter_get_action_type(fid);

			if (action == command::SPELL) {
				speed_mod *= 0.5f;
			} else if (action == command::PARRY) {
				speed_mod *= 0.25f;
			} else if (action == command::CHARGE_PREPARATION) {
				speed_mod *= 0.25f;
			} else if (action == command::INVISIBILITY_PREPARATION) {
				speed_mod *= 0.1f;
			} else if (action == command::ATTACK) {
				speed_mod *= 0.3f;
			}

			game.state.fighter_set_action_timer(fid, std::max(0.f, progress - edt));

			bool completed = progress - edt <= 0.f;

			if (action == command::ATTACK) {
				execute_attack(game, dt, completed, fid);
			} else if (action == command::CHARGE_PREPARATION) {
				execute_charge(game, dt, completed, fid);
			} else if (action == command::JUMP_BEHIND) {
				execute_jump_behind(game, dt, completed, fid);
			} else {
				if (completed) {
					if (action == command::SPELL && selected) {
						shoot_spell(game, fid, selected);
					} else if (action == command::INVISIBILITY_PREPARATION) {
						event_notification(game, fid, update::EVENT_START_INVISIBILITY);
						game.state.fighter_set_invisible_timer(fid, INVISIBILITY_DURATION);
					}

					game.state.fighter_set_action_type(fid, command::MOVE);
				}
			}
		}


		float no_damage = game.state.fighter_get_no_damage_timer(fid);
		if (no_damage > 0.f) {
			game.state.fighter_set_no_damage_timer(fid, std::max(0.f, no_damage - dt));
		}
		auto invisible = game.state.fighter_get_invisible_timer(fid);
		if (invisible > 0.f) {
			speed_mod *= 2.f;
			game.state.fighter_set_invisible_timer(fid, std::max(0.f, invisible - dt));
		}
		float cooldown = game.state.fighter_get_parry_cooldown_timer(fid);
		if (cooldown > 0.f) {
			game.state.fighter_set_parry_cooldown_timer(fid, std::max(0.f, cooldown - dt));
		} 
		if (game.state.fighter_get_action_type(fid) == command::MOVE) {
			x = game.state.fighter_get_x(fid);
			y = game.state.fighter_get_y(fid);
			x += dx * dt * speed_mod;
			y += dy * dt * speed_mod;
			game.state.fighter_set_x(fid, x);
			game.state.fighter_set_y(fid, y);
		}
		
		x = game.state.fighter_get_x(fid);
		y = game.state.fighter_get_y(fid);
		auto norm_f = sqrt(x * x + y * y);
		if (norm_f > 1.f) {
			x /= norm_f;
			y /= norm_f;
		}
		game.state.fighter_set_x(fid, x);
		game.state.fighter_set_y(fid, y);
	});

	std::vector<dcon::projectile_id> marked_for_deletion_projectile;

	game.state.for_each_projectile([&](auto proj){
		auto x = game.state.projectile_get_x(proj);
		auto y = game.state.projectile_get_y(proj);

		auto homing = game.state.projectile_get_homing_target(proj);
		auto target = game.state.homing_target_get_target(homing);

		auto tx = game.state.fighter_get_x(target);
		auto ty = game.state.fighter_get_y(target);

		auto dx = tx - x;
		auto dy = ty - y;

		auto norm = sqrt(dx * dx + dy * dy);


		if (norm < dt) {
			if (game.state.fighter_get_no_damage_timer(target) == 0.f) {
				game.state.fighter_set_hp(
					target,
					std::max(0, game.state.fighter_get_hp(target) - 1)
				);
			}
			marked_for_deletion_projectile.push_back(proj);
		} else {
			game.state.projectile_set_x(proj, x + dx / norm * dt);
			game.state.projectile_set_y(proj, y + dy / norm * dt);
		}
	});

	for (int i = 0; i < int(marked_for_deletion_projectile.size()); ++i) {
		game.state.delete_projectile(marked_for_deletion_projectile[i]);
	}


	std::vector<dcon::fighter_id> marked_for_deletion_fighter;

	game.state.for_each_fighter([&](auto fid){
		if (game.state.fighter_get_hp(fid) <= 0) {
			marked_for_deletion_fighter.push_back(fid);
		}
	});

	for (int i = 0; i < int(marked_for_deletion_fighter.size()); ++i) {
		auto fid = marked_for_deletion_fighter[i];
		auto pid = game.state.fighter_get_controller_from_player_control(fid);
		game.state.delete_fighter(fid);
		game.state.player_set_know_my_body(pid, false);
		auto loc = game.state.player_get_location(pid);
		game.state.delete_location(loc);
		event_notification_to_player(game, pid, update::EVENT_PLAYER_DIED);
	}
}

static game_session game { };

int consume_command(game_session& game, int connection, command::data command) {

	printf("new command %d\n", command.command_type);

	dcon::player_id id { (dcon::player_id::value_base_t) command.actor };

	if (!game.state.player_is_valid(id)) {
		return 0;
	}

	if (game.state.player_get_connection(id) != connection) {
		return 0;
	}

	dcon::player_control_id control = game.state.player_get_player_control(id);

	if (!control) {
		return 0;
	}

	if (command.command_type == command::KNOW_MYSELF) {
		game.state.player_set_know_myself(id, true);
		return 0;
	}

	if (command.command_type == command::KNOW_MY_BODY) {
		game.state.player_set_know_my_body(id, true);
		return 0;
	}

	dcon::fighter_id fighter = game.state.player_control_get_controlled(control);
	auto location = game.state.player_get_location(id);
	auto lobby = game.state.location_get_where(location);

	if (!fighter) {
		if (!lobby) {
			if (command.command_type == command::JOIN_LOBBY) {
				lobby = dcon::room_id { command.command_data };

				if (!game.state.room_is_valid(lobby)) {
					return 0;
				}

				game.state.force_create_location(id, lobby);

				event_notification_to_player(game, id, update::EVENT_JOIN_LOBBY);

				return 0;
			}
		} else {
			if (command.command_type == command::SELECT_CLASS) {
				if (command.command_data >= command::CLASS_TOTAL) {
					return 0;
				}

				auto fid = game.state.create_fighter();
				game.state.fighter_set_character_class(fid, command.command_data);
				game.state.fighter_set_hp(fid, 5);
				game.state.force_create_player_control(id, fid);
				game.state.player_set_know_myself(id, false);
				game.state.player_set_know_my_body(id, false);

				event_notification_to_player(game, id, update::EVENT_JOIN_BATTLE);

				return 0;
			}
		}

		return 0;
	}

	if (game.state.fighter_get_hp(fighter) <= 0) {
		return 0;
	}

	if (
		command.command_type == command::MOVE
		&& game.state.fighter_get_charge_timer(fighter) <= 0.f
	) {
		game.state.fighter_set_tx(fighter, command.target_x);
		game.state.fighter_set_ty(fighter, command.target_y);
	} else if (
		command.command_type == command::SPELL
		&& !is_busy(game, fighter)
	) {

		if (game.state.fighter_get_character_class(fighter) != command::CLASS_MAGE) {
			return 0;
		}

		dcon::fighter_id selected { (dcon::fighter_id::value_base_t) command.target_actor };

		// validation

		if (!game.state.fighter_is_valid(selected)) {
			return 0;
		}


		auto target_control = game.state.fighter_get_player_control(selected);
		auto target_player = game.state.player_control_get_controller(target_control);
		auto target_location = game.state.player_get_location(target_player);
		auto target_room = game.state.location_get_where(target_location);

		if (target_room != lobby) {
			return 0;
		}
		if (game.state.fighter_get_invisible_timer(selected) > 0.f) {
			return 0;
		}

		printf("start casting\n");
		event_notification(game, fighter, update::EVENT_START_CAST, SPELL_PREPARATION, 0.f);
		game.state.fighter_set_action_timer(fighter, SPELL_PREPARATION);
		game.state.fighter_set_action_type(fighter, command::SPELL);
		game.state.force_create_selection(selected, fighter);
	} else if (command.command_type == command::SELECTION) {
		dcon::fighter_id selected { (dcon::fighter_id::value_base_t) command.target_actor };
		if (!game.state.fighter_is_valid(selected)) {
			return 0;
		}

		if (!can_be_selected(game, fighter, selected)) {
			return 0;
		}

		game.state.force_create_selection(selected, fighter);
	} else if (
		command.command_type == command::PARRY
		&& game.state.fighter_get_no_damage_timer(fighter) == 0.f
		&& game.state.fighter_get_parry_cooldown_timer(fighter) == 0.f
		&& !is_busy(game, fighter)
	) {
		game.state.fighter_set_action_timer(fighter, PARRY_PREPARATION);
		game.state.fighter_set_no_damage_timer(fighter, PARRY_PREPARATION);
		game.state.fighter_set_parry_cooldown_timer(fighter, 1.f);
		event_notification(game, fighter, update::EVENT_START_PARRY, PARRY_PREPARATION, 0.f);
		event_notification(game, fighter, update::EVENT_NO_DAMAGE, PARRY_PREPARATION, 0.f);
		game.state.fighter_set_action_type(fighter, command::PARRY);
	} else if (
		command.command_type == command::ATTACK
		&& !is_busy(game, fighter)
	) {
		if (game.state.fighter_get_character_class(fighter) == command::CLASS_MAGE) {
			return 0;
		}

		event_notification(game, fighter, update::EVENT_START_ATTACK, ATTACK_PREPARATION, 0.f);
		game.state.fighter_set_action_timer(fighter, ATTACK_PREPARATION);
		game.state.fighter_set_action_type(fighter, command::ATTACK);
	} else if (
		command.command_type == command::JUMP_BEHIND
		&& !is_busy(game, fighter)
	) {
		if (game.state.fighter_get_character_class(fighter) != command::CLASS_ROGUE) {
			return 0;
		}

		event_notification(game, fighter, update::EVENT_START_ATTACK, ATTACK_PREPARATION, 0.f);
		game.state.fighter_set_action_timer(fighter, 0.1f);
		game.state.fighter_set_action_type(fighter, command::JUMP_BEHIND);
	} else if (
		command.command_type == command::INVISIBILITY_PREPARATION
		&& !is_busy(game, fighter)
	) {
		if (game.state.fighter_get_character_class(fighter) != command::CLASS_ROGUE) {
			return 0;
		}

		event_notification(
			game,
			fighter,
			update::EVENT_START_INVISIBILITY_PREPARATION,
			INVISIBILITY_PREPARATION, 0.f
		);
		game.state.fighter_set_action_timer(fighter, INVISIBILITY_PREPARATION);
		game.state.fighter_set_action_type(fighter, command::INVISIBILITY_PREPARATION);
	} else if (
		command.command_type == command::CHARGE_PREPARATION
		&& !is_busy(game, fighter)
	) {
		if (game.state.fighter_get_character_class(fighter) != command::CLASS_WARRIOR) {
			return 0;
		}
		printf("charge\n");
		event_notification(
			game,
			fighter,
			update::EVENT_START_CHARGE,
			CHARGE_PREPARATION, 0.f
		);
		game.state.fighter_set_action_timer(fighter, CHARGE_PREPARATION);
		game.state.fighter_set_action_type(fighter, command::CHARGE_PREPARATION);
	}

	return 0;
}

int read_from_connection (game_session& game, int connection) {
	char buffer[256];
	int nbytes;

	nbytes = read(connection, buffer, 256);

	if (nbytes <= 0) {
		perror("Read failed");
		// connection ended
		// delete players with this connection
		std::vector<dcon::player_id> players_to_delete;
		game.state.for_each_player([&](auto pid) {
			if (game.state.player_get_connection(pid) == connection)
				players_to_delete.push_back(pid);
		});


		for (int i = 0; i < (int)players_to_delete.size(); ++i) {
			auto pid = players_to_delete[i];
			auto control = game.state.player_get_player_control(pid);
			auto fighter = game.state.player_control_get_controlled(control);
			event_notification(game, fighter, update::EVENT_LEFT_GAME);
			printf("delete player %d\n", pid.index());
			game.state.delete_player(pid);
			if (fighter) {
				game.state.delete_fighter(fighter);
			}
		}

		return -1;
	} else {
		command::data command {};
		memcpy(&command, buffer, sizeof(command::data));

		return consume_command(game, connection, command);
	}

	return 0;
}

void sigpipe_handler(int unused)
{

}


int main(int argc, char const* argv[]) {
	struct sigaction action { { sigpipe_handler } };

	sigaction(SIGPIPE, &action, NULL);


	if (argc == 1) {
		std::cout << "Port is missing\n";
		exit(EXIT_FAILURE);
	}

	errno = 0;
	const long port = strtol(argv[1], nullptr, 10);

	std::cout << "Attempt to run server at " << port << "\n";

	int tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
	if(tcp_socket < 0) {
		perror("TCP socket failed");
		exit(EXIT_FAILURE);
	}

	int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (udp_socket < 0) {
		perror("UDP socket failed");
		exit(EXIT_FAILURE);
	}

	int opt = 1;
	if(setsockopt(
		tcp_socket,
		SOL_SOCKET,
		SO_REUSEADDR | SO_REUSEPORT,
		&opt,
		sizeof(opt)
	)) {
		perror("setsockopt failed");
		exit(EXIT_FAILURE);
	}

	sockaddr_in address;
	socklen_t address_length = sizeof(address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	if(bind(tcp_socket, (sockaddr *) &address, address_length) < 0) {
		perror("TCP bind failed");
		exit(EXIT_FAILURE);
	}

	if(bind(udp_socket, (sockaddr *) &address, address_length) < 0) {
		perror("UDP bind failed");
		exit(EXIT_FAILURE);
	}

	if(listen(tcp_socket, 5) < 0 ) {
		perror("TCP listen failed");
		exit(EXIT_FAILURE);
	}

	std::cout << "Listening\n";

	fd_set active_connections;
	fd_set read_connections;

	FD_ZERO(&active_connections);
	FD_SET(tcp_socket, &active_connections);

	fd_set udp_singleton;
	fd_set udp_select_singleton;
	FD_ZERO(&udp_singleton);
	FD_SET(udp_socket, &udp_singleton);

	sockaddr_in client_address;

	int i;
	size_t connection_address_size;

	struct timeval timeout { 0, 0 };

	auto now = std::chrono::system_clock::now();

	auto last_server_state_update = now;
	bool update_requested = false;

	int updated = 0;

	game.state.create_room();
	game.state.create_room();
	auto test_room = game.state.create_room();
	auto test_fighter = game.state.create_fighter();
	auto test_player = game.state.create_player();
	game.state.fighter_set_hp(test_fighter, 20);
	game.state.fighter_set_controller_from_player_control(test_fighter, test_player);
	game.state.player_set_where_from_location(test_player, test_room);


//	int counter = 1000 * 30;

	int32_t timestamp = 0;

	ankerl::unordered_dense::map<in_addr_t, sockaddr_in> internet_address_to_udp_address {};


  	char udp_buffer[1024] = {0};

	while (1) {

		{
			udp_select_singleton = udp_singleton;

			auto udp_has_message = select(
				FD_SETSIZE,
				&udp_select_singleton,
				NULL,
				NULL,
				&timeout
			);

			if (udp_has_message < 0) {
				perror("Select error (udp)");
				exit(EXIT_FAILURE);
			}

			if (udp_has_message > 0) {
				// handle UDP "subscriptions"
				connection_address_size = sizeof(client_address);
				auto status = recvfrom(udp_socket, udp_buffer, 1024, 0, (struct sockaddr*)&client_address, (socklen_t *) &connection_address_size);

				if (status >= 0) {
					printf("got UDP message\n");
					internet_address_to_udp_address[client_address.sin_addr.s_addr] = client_address;
				}
			}
		}

//		counter--;
		read_connections = active_connections;

		// retrieve sockets which demand attention
		if (updated = select(FD_SETSIZE, &read_connections, NULL, NULL, &timeout); updated < 0) {
			perror("Select error");
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < FD_SETSIZE && updated > 0; ++i) {
			if (!FD_ISSET(i, &read_connections)) {
				continue;
			}

			if (i == tcp_socket) {
				// connection requests
				connection_address_size = sizeof(client_address);

				int new_connection = accept(
					tcp_socket,
					(sockaddr *) & client_address,
					(socklen_t *) &connection_address_size
				);

				if (new_connection < 0) {
					perror("Accept connection error");
					exit(EXIT_FAILURE);
				}

				fprintf(
					stderr,
					"SERVER: NEW CONNECTION\n"
				);

				if (game.state.player_size() > 100) {
					// Deny connections when there are 100 players
					continue;
				}
				auto pid = game.state.create_player();
				game.state.player_set_connection(pid, new_connection);
				game.state.player_set_address(pid, client_address.sin_addr.s_addr);
				FD_SET(new_connection, &active_connections);
			} else {
				// data from established connection

				if (read_from_connection(game, i) < 0) {
					// invalid data
					printf("close %d\n", i);
					close(i);
					FD_CLR(i, &active_connections);
				}
			}
		}

		auto then = std::chrono::system_clock::now();

		auto duration_game_state_update = std::chrono::duration_cast<std::chrono::microseconds> (
			then - now
		);
		auto duration_network_update = std::chrono::duration_cast<std::chrono::microseconds> (
			then - last_server_state_update
		);
		if (duration_network_update.count() > 1000 * 1000 / 30) {
			// printf("send update %ld\n", duration_network_update.count() / 1000 / 1000);
			last_server_state_update = then;
			timestamp++;
			game.state.for_each_player([&](auto dest) {
				auto connection = game.state.player_get_connection(dest);
				if (!FD_ISSET(connection, &active_connections)) {
					return;
				}

				auto location = game.state.player_get_location(dest);
				auto lobby = game.state.location_get_where(location);
				auto dest_control = game.state.player_get_player_control(dest);
				auto dest_fid = game.state.player_control_get_controlled(dest_control);
				auto internet_address = game.state.player_get_address(dest);

				auto& udp_address_iterator = internet_address_to_udp_address[internet_address];

				game.state.room_for_each_location(lobby, [&](auto player_location){
					auto player = game.state.location_get_who(player_location);
					auto control = game.state.player_get_player_control(player);
					auto fid = game.state.player_control_get_controlled(control);

					if (
						game.state.fighter_get_invisible_timer(fid) > 0.f
						&& fid
						&& player != dest
					) {
						return;
					}

					update::udp_data to_send {};
					to_send.timestamp = timestamp;
					to_send.id = fid.index();
					to_send.x = game.state.fighter_get_x(fid);
					to_send.y = game.state.fighter_get_y(fid);
					to_send.z = game.state.fighter_get_direction(fid);
					to_send.update_type = update::FIGHTER;
					to_send.additional_data = game.state.fighter_get_hp(fid);
					to_send.belongs_to = game.state.fighter_get_character_class(fid);

					if (
						game.state.fighter_get_invisible_timer(fid) > 0.f
						&& fid
						&& player == dest
					) {
						to_send.flags |= 1;
					}

					if (
						fid
						&& game.state.fighter_get_stunned_timer(fid) > 0.f
					) {
						to_send.flags |= 2;
					}

					sendto(
						udp_socket,
						(char*)&to_send,
						sizeof(update::udp_data),
						0,
						(sockaddr *) &(udp_address_iterator),
						sizeof(client_address)
					);
				});

				game.state.room_for_each_projectile_location(lobby, [&](auto proj_location){
					auto proj = game.state.projectile_location_get_what(proj_location);

					update::data to_send {};
					to_send.id = proj.index();
					to_send.x = game.state.projectile_get_x(proj);
					to_send.y = game.state.projectile_get_y(proj);
					to_send.update_type = update::SPELL;


					send(connection, (char*)&to_send, sizeof(update::data), 0);
				});

				if (
					!game.state.player_get_know_myself(dest)
				) {
					printf("Notify player of their identity\n");
					update::data to_send {};
					to_send.id = dest.index();
					to_send.belongs_to = 1;
					to_send.update_type = update::SEND_ID;
					send(connection, (char*)&to_send, sizeof(update::data), 0);
				}

				if (
					!game.state.player_get_know_my_body(dest)
					&& dest_fid
				) {
					printf("Notify player of their fighter\n");
					update::data to_send {};
					to_send.id = dest_fid.index();
					to_send.belongs_to = 1;
					to_send.update_type = update::SEND_FIGHTER_ID;
					send(connection, (char*)&to_send, sizeof(update::data), 0);
				}
			});
		}

		if (duration_game_state_update.count() > 1000 * 1000 / 200) {
			update_game_state(game, duration_game_state_update);
			now = then;
		}

		usleep(10);
	}
}
