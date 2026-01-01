#include <cmath>

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

constexpr float PARRY_PREPARATION = 0.3f;
constexpr float PARRY_DURATION = 0.3f;

constexpr float INVISIBILITY_PREPARATION = 2.f;
constexpr float INVISIBILITY_DURATION = 13.f;

constexpr float SPELL_PREPARATION = 1.f;

constexpr float CHARGE_PREPARATION = 0.5f;
constexpr float CHARGE_DURATION = 2.f;

constexpr float ATTACK_PREPARATION = 0.2f;

constexpr float ATTACK_RANGE = 0.1f;

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

	return sqrt(dx * dx + dy * dy);
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


void charge(game_session& game, dcon::fighter_id origin, dcon::fighter_id target, float dt, float speed_mod) {
	game.state.fighter_set_charge_timer(target, 
		std::max(0.f, game.state.fighter_get_charge_timer(target) - dt)
	);
	
	auto x = game.state.fighter_get_x(origin);
	auto y = game.state.fighter_get_y(origin);

	auto tx = game.state.fighter_get_x(target);
	auto ty = game.state.fighter_get_y(target);
	game.state.fighter_set_tx(origin, 0.f);
	game.state.fighter_set_ty(origin, 0.f);
	auto dx = tx - x;
	auto dy = ty - y;
	auto norm = sqrt(dx * dx + dy * dy);
	if (norm > ATTACK_RANGE * 0.9) {
		dx /= norm;
		dy /= norm;
	} else {
		game.state.fighter_set_charge_timer(origin, 0.f);
		game.state.fighter_set_stunned_timer(target, MEDIUM_STUN_DURATION);
	}

	game.state.fighter_set_x(origin, x + dx * dt * speed_mod);
	game.state.fighter_set_y(origin, y + dy * dt * speed_mod);
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
		auto norm = sqrt(dx * dx + dy * dy);
		if (norm > dt) {
			dx /= norm;
			dy /= norm;
		}

		float speed_mod = 0.4f;
		float progress_mod = 1.f;
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
		
		auto stunned = game.state.fighter_get_stunned_timer(fid);
		if (stunned > 0.f) {
			game.state.fighter_set_stunned_timer(fid, std::max(0.f, stunned - dt));
			speed_mod = 0.f;
			progress_mod = 0.f;
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

			// during action

			if (progress - edt <= 0.f) {
				// on action end
				if (selected) {
					if (action == command::SPELL) {
						shoot_spell(game, fid, selected);
					} else if (action == command::CHARGE_PREPARATION) {
						game.state.fighter_set_charge_timer(
							fid, CHARGE_DURATION
						);
					} else if (action == command::ATTACK) {
						auto damage = 1;
						if (game.state.fighter_get_invisible_timer(fid) > 0.f) {
							damage *= 2;
						}
						if (distance(game, fid, selected) < ATTACK_RANGE) {
							game.state.fighter_set_hp(selected, 
								std::max(0, game.state.fighter_get_hp(selected) - damage)
							);
							game.state.fighter_set_invisible_timer(fid, 0.f);
						}
					}
				}
				
				if (action == command::PARRY) {
					event_notification(
						game, fid, update::EVENT_NO_DAMAGE, PARRY_DURATION, 0.f
					);
					game.state.fighter_set_no_damage_timer(fid, PARRY_DURATION);
				} else if (action == command::INVISIBILITY_PREPARATION) {
					event_notification(game, fid, update::EVENT_START_INVISIBILITY);
					game.state.fighter_set_invisible_timer(fid, INVISIBILITY_DURATION);
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
		auto charging = game.state.fighter_get_charge_timer(fid);
		if (charging == 0.f) {
			x += dx * dt * speed_mod;
			y += dy * dt * speed_mod;

			auto norm_f = sqrt(x * x + y * y);
			if (norm_f > 1.f) {
				x /= norm_f;
				y /= norm_f;
			}

			game.state.fighter_set_x(fid, x);
			game.state.fighter_set_y(fid, y);
		} else {
			charge(game, fid, selected, dt, speed_mod);	
		}
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

	//for (int i = 0; i < int(marked_for_deletion_fighter.size()); ++i) {
	//	game.state.delete_fighter(marked_for_deletion_fighter[i]);
	//}
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
		&& !is_busy(game, fighter)
	) {
		event_notification(game, fighter, update::EVENT_START_PARRY, PARRY_PREPARATION, 0.f);
		game.state.fighter_set_action_timer(fighter, PARRY_PREPARATION);
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
			if (fighter) game.state.delete_fighter(fighter);
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

	int server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if(server_socket < 0) {
		perror("Socket failed");
		exit(EXIT_FAILURE);
	}
	
	int opt = 1;
	if(setsockopt(
		server_socket, 
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

	if(bind(server_socket, (sockaddr *) &address, address_length) < 0) {
		perror("Bind failed");
		exit(EXIT_FAILURE);
	}

	if(listen(server_socket, 5) < 0 ) {
		perror("Listen failed");
		exit(EXIT_FAILURE);
	}

	std::cout << "Listening\n";
	
	fd_set active_connections;
	fd_set read_connections;

	FD_ZERO(&active_connections);
	FD_SET(server_socket, &active_connections);

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
	game.state.create_room();
		
//	int counter = 1000 * 30;

	while (1) {
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

			if (i == server_socket) {
				// connection requests
				connection_address_size = sizeof(client_address);

				int new_connection = accept(
					server_socket,
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
		if (duration_network_update.count() > 1000 * 1000 / 75) {
			game.state.for_each_player([&](auto dest) {
				auto connection = game.state.player_get_connection(dest);
				if (!FD_ISSET(connection, &active_connections)) {
					return;
				}

				auto location = game.state.player_get_location(dest);
				auto lobby = game.state.location_get_where(location);
				auto dest_control = game.state.player_get_player_control(dest);
				auto dest_fid = game.state.player_control_get_controlled(dest_control);

				game.state.room_for_each_location(lobby, [&](auto player_location){
					auto player = game.state.location_get_who(player_location);
					auto control = game.state.player_get_player_control(player);
					auto fid = game.state.player_control_get_controlled(control);
					
					if (
						game.state.fighter_get_invisible_timer(fid) > 0.f
						&& player != dest
					) {
						return;
					}

					update::data to_send {};
					to_send.id = fid.index();
					to_send.x = game.state.fighter_get_x(fid);
					to_send.y = game.state.fighter_get_y(fid);
					to_send.update_type = update::FIGHTER;
					to_send.additional_data = game.state.fighter_get_hp(fid);
					to_send.belongs_to = game.state.fighter_get_character_class(fid);

					send(connection, (char*)&to_send, sizeof(update::data), 0);
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
			last_server_state_update = then;
		}

		if (duration_game_state_update.count() > 1000 * 1000 / 200) {
			update_game_state(game, duration_game_state_update);
			now = then;
		}

		usleep(10);
	}
}
