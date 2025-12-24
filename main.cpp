#include "iostream"
#include "data.hpp"

struct game_session {
        dcon::data_container state{};
};


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

static game_session game {};

int main() {
        new_player(game);
        std::cout << "not implemented\n";
}