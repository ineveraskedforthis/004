#include "iostream"
#include "data.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <signal.h>
#include <chrono>
#include <vector>

struct game_session {
        dcon::data_container state{};
};


//inline constexpr float base_speed = 0.01f;

void update_game_state(game_session& game, std::chrono::microseconds last_tick) {
	float dt = float(last_tick.count()) / 1'000'000.f;

	game.state.for_each_fighter([&](auto fid){
		auto x = game.state.fighter_get_x(fid);
		auto y = game.state.fighter_get_y(fid);
		auto tx = game.state.fighter_get_tx(fid);
		auto ty = game.state.fighter_get_ty(fid);
		
		auto dx = tx - x;
		auto dy = ty - y;
	
		auto norm = sqrt(dx * dx + dy * dy);
		if (norm > dt) {
			dx /= norm;
			dy /= norm;
		}

		x += dx * dt;
		y += dy * dt;

		auto norm_f = sqrt(x * x + y * y);
		if (norm_f > 1.f) {
			x /= norm_f;
			y /= norm_f;
		}

		game.state.fighter_set_x(fid, x);
		game.state.fighter_set_y(fid, y);
	});
}

dcon::player_id new_player(game_session& game) {
        auto id = game.state.create_fighter();
        game.state.fighter_set_rotation(id, 0.f);
        game.state.fighter_set_x(id, 0.f);
        game.state.fighter_set_y(id, 0.f);
        game.state.fighter_set_size(id, 10.f);

        auto pid = game.state.create_player();

        game.state.force_create_player_control(pid, id);

        return pid;
}

static game_session game { };

namespace command {

uint8_t MOVE = 0;
uint8_t SPELL = 1;

struct data {
	int32_t actor;
	float target_x;
	float target_y;
	uint8_t command_type;
	uint8_t padding[3];
};

}

namespace update {
uint8_t FIGHTER = 0;
uint8_t SPELL = 1;
uint8_t SEND_ID = 2;
struct data {
	int32_t id;
	float x;
	float y;
	uint8_t update_type;
	uint8_t belongs_to;
	uint8_t padding[2];
};
}

static_assert(sizeof(command::data) == 4 * 4);
static_assert(sizeof(command::data) < 256);

int consume_command(game_session& game, int connection, command::data command) {
	
	printf("new command\n");

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

	dcon::fighter_id fighter = game.state.player_control_get_controlled(control);

	if (!fighter) {
		fighter = game.state.create_fighter();
		game.state.force_create_player_control(id, fighter);
	}

	if (command.command_type == command::MOVE) {
		game.state.fighter_set_tx(fighter, command.target_x);
		game.state.fighter_set_ty(fighter, command.target_y);
	} else if (command.command_type == command::SPELL) {
		game.state.fighter_set_spell_target_x(fighter, command.target_x);
		game.state.fighter_set_spell_target_y(fighter, command.target_y);
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
			printf("delete player %d\n", players_to_delete[i].index());
			game.state.delete_player(players_to_delete[i]);
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
	

        new_player(game);

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
				auto fid = game.state.create_fighter();
				game.state.force_create_player_control(pid, fid);
				
				game.state.player_set_knows_themselves(pid, false);

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
		
		auto duration = std::chrono::duration_cast<std::chrono::microseconds> (then - now);

		
		auto duration_update = std::chrono::duration_cast<std::chrono::microseconds> (
			then - last_server_state_update
		);
		if (duration_update.count() > 1000 * 1000 / 30) {
			game.state.for_each_player([&](auto dest) {
				auto connection = game.state.player_get_connection(dest);
				if (!FD_ISSET(connection, &active_connections)) {
					return;
				}

				game.state.for_each_fighter([&](auto fid){
					update::data to_send {};
					to_send.id = fid.index();
					to_send.x = game.state.fighter_get_x(fid);
					to_send.y = game.state.fighter_get_y(fid);
					to_send.update_type = update::FIGHTER;
					
					send(connection, (char*)&to_send, sizeof(update::data), 0);
				});			
					
				if (
					true
				) {
					printf("Notify player of their identity\n");
					update::data to_send {};
					to_send.id = dest.index();
					to_send.belongs_to = 1;
					to_send.update_type = update::SEND_ID;
					send(connection, (char*)&to_send, sizeof(update::data), 0);
					game.state.player_set_knows_themselves(dest, true);
				}
				
			});
			last_server_state_update = then;
			update_game_state(game, duration);
			now = then;
		}

		usleep(100);
	}
}
