#pragma once

namespace command {

uint8_t MOVE = 0;
uint8_t SPELL = 1;
uint8_t SELECTION = 2;
uint8_t PARRY = 3;
uint8_t SELECT_CLASS = 4;
uint8_t JOIN_LOBBY = 5;
uint8_t KNOW_MYSELF = 6;

uint8_t CLASS_MAGE = 0;
uint8_t CLASS_WARRIOR = 1;
uint8_t CLASS_ROGUE = 2;
uint8_t CLASS_TOTAL = 3;

struct data {
	int32_t actor;
	int32_t target_actor;	
	float target_x;
	float target_y;
	uint8_t command_type;
	uint8_t command_data;
	uint8_t padding[2];
};

}

static_assert(sizeof(command::data) == 4 * 5);

namespace update {
uint8_t FIGHTER = 0;
uint8_t SPELL = 1;
uint8_t SEND_ID = 2;
uint8_t EVENT = 3;

uint8_t EVENT_NONE = 0;
uint8_t EVENT_START_CAST = 1;
uint8_t EVENT_START_PARRY = 2;
uint8_t EVENT_NO_DAMAGE = 3;
uint8_t EVENT_JOIN_LOBBY = 4;
uint8_t EVENT_JOIN_BATTLE = 5;
uint8_t EVENT_LEFT_GAME = 6;

struct data {
	int32_t id;
	float x;
	float y;
	uint8_t update_type;
	uint8_t belongs_to;
	uint8_t additional_data;
	uint8_t event_type;
};
}

static_assert(sizeof(update::data) == 4 * 4);

constexpr int buffer_size = 256;

static_assert(sizeof(command::data) < buffer_size);
static_assert(sizeof(update::data) < buffer_size);


