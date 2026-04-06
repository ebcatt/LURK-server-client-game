/*  After testing notes
    - make a healing room
    - map does work just not on windows machines
    - rework looting
    - rework removing player from the room (by sending the new room number back)
*/

// lurk_server.cpp
#include <sys/socket.h>
#include <sys/types.h>
#include <ctime>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* Eventually
//mapping stuff
#include "lurk_server.hpp"
#include "lurk_map.h"
*/
// --- LURK message structs, packed ---
struct version_message {
    uint8_t type = 14;
    uint8_t major = 2;
    uint8_t minor = 3;
    uint16_t extlen = 0;
} __attribute__((packed));

// Describes the game
struct game_message {
    uint8_t type;
    uint16_t initial_points;
    uint16_t stat_limit;
    uint16_t description_length;
} __attribute__((packed));

// Character flag bitmasks
#define FLAG_ALIVE        0x80
#define FLAG_JOIN_BATTLE  0x40
#define FLAG_MONSTER      0x20
#define FLAG_STARTED      0x10
#define FLAG_READY        0x08

// Structure for the player
struct our_player {
    uint8_t type;              // player type
    char name[32];             // player name
    uint8_t flags;
    uint16_t attack;
    uint16_t defence;
    uint16_t regen;
    int16_t health;
    uint16_t gold;
    uint16_t room;
    uint16_t description_length;
    int fd;                    // socket for player
    // description follows
} __attribute__((packed));

// Allows for fighting
struct fight {
    uint8_t type;
} __attribute__((packed));

// PVP fighting
struct PVP_fight {
    uint8_t type;
    char target[32];
} __attribute__((packed));

// Looting
struct loot {
    uint8_t type;
    char target[32];
} __attribute__((packed));

// Describes the room
struct room_message {
    uint8_t type;
    uint16_t room_number;
    char room_name[32];
    uint16_t description_length;
} __attribute__((packed));

// Room Connection messages
struct connection_message {
    uint8_t type;
    uint16_t room_number;
    char room_name[32];
    uint16_t description_length;
} __attribute__((packed));

// Allows for changing rooms
struct change_room {
    uint8_t type;         // type 2 = room change
    uint16_t room_number;
} __attribute__((packed));

// Allows for messages in game
struct message {
    uint8_t type = 1;
    uint16_t message_length;
    char recipient[32];
    char sender[30];
    uint8_t end_marker[2];
    // message text here
} __attribute__((packed));

// Lurk Error messaging
struct error_message {
    uint8_t type;
    uint8_t code;
    uint16_t len;
    // error follows
} __attribute__((packed));

//Handling Leave struct
struct leave {
    uint8_t type;
}__attribute__((packed));

// LURK Start Message
struct start_message {
    uint8_t type;
} __attribute__((packed));

// Accept Message for client
struct accept_message {
    uint8_t type;          // always 8
    uint8_t accepted_type;
} __attribute__((packed));

//helper for error messages
void send_error(int fd, uint8_t code, const char* msg) {
    struct error_message e;
    e.type = 7;
    e.code = code;
    e.len = strlen(msg);
    write(fd, &e, sizeof(e));
    write(fd, msg, e.len);
}

// Forward declarations for helpers
void send_character_snapshot(int fd, our_player* occupant, const char* desc);


// Room Structures
#define MAX_ROOMS        10   // total number of rooms for map
#define MAX_OCCUPANTS    32   // max players in one room
#define MAX_CONNECTIONS  8

typedef struct {
    uint16_t room_number;
    char name[32];
    char* desc;  // description of connection
} Connection;

typedef struct {
    uint16_t room_number;
    char name[32];
    char* room_description;
    Connection connections[MAX_CONNECTIONS];      // Max connections
    int num_connections;
    struct our_player* occupants[MAX_OCCUPANTS];  // pointers to players in room
    int occupant_count;                           // number of players in room
    pthread_mutex_t lock;                         // mutex for safely modifying room occupants
} Rooms;

// Global Room List
Rooms game_rooms[MAX_ROOMS];

// --- Global mutex for player list ---
pthread_mutex_t players_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Player storage ---
#define MAX_PLAYERS 32
our_player* players[MAX_PLAYERS] = {0};
const char* player_descs[MAX_PLAYERS];

// --- Bot definitions ---
#define MAX_MONSTERS 26
our_player monsters[MAX_MONSTERS];
const char* monster_descs[MAX_MONSTERS];

// For monster stats and descriptions
typedef struct {
    const char* name;
    const char* desc;
    uint32_t health;
    uint16_t attack;
    uint16_t defence;
} MonsterType;

// Base Monsters
MonsterType monster_types[] = {
    {"Bat", "A screeching bat flutters around.", 10, 5, 2},
    {"Mummy", "A slow-moving mummy lurks here.", 25, 10, 5},
    {"Goblin", "A goblin eyes you hungrily.", 50, 20, 10}
};

// Helper function — create monsters
void create_monster(MonsterType* type, int room, int count, int& idx, int& type_counter) {
    for (int i = 0; i < count; i++) {
        sprintf(monsters[idx].name, "%s%d", type->name, type_counter + 1);  // unique name
        monsters[idx].flags = FLAG_ALIVE | FLAG_MONSTER;
        monsters[idx].health = type->health;
        monsters[idx].attack = type->attack;
        monsters[idx].defence = type->defence;
        monsters[idx].regen = 1;
        monsters[idx].gold = 5 + (rand() % 71);  // random 5–75
        monsters[idx].room = room;
        monsters[idx].description_length = strlen(type->desc);
        monster_descs[idx] = strdup(type->desc);

        // Add monster to room occupant list
        pthread_mutex_lock(&game_rooms[room - 1].lock);
        game_rooms[room - 1].occupants[game_rooms[room - 1].occupant_count++] = &monsters[idx];
        pthread_mutex_unlock(&game_rooms[room - 1].lock);

        idx++;
        type_counter++;
    }
}

// Create monsters in rooms
void init_monsters() {
    int idx = 0;
    int bat_counter = 0;
    int mummy_counter = 0;
    int goblin_counter = 0;

    // Cave Entrance
    create_monster(&monster_types[0], 1, 2, idx, bat_counter);

    // Crystal Cavern: 3 Bats + 1 Mummy
    create_monster(&monster_types[0], 3, 3, idx, bat_counter);
    create_monster(&monster_types[1], 3, 1, idx, mummy_counter);

    // Goblin Nest: 10 Goblins
    create_monster(&monster_types[2], 4, 10, idx, goblin_counter);

    // Ancient Shrine: 5 Mummies
    create_monster(&monster_types[1], 8, 5, idx, mummy_counter);

    // Treasure Vault: 3 Goblins + 2 Mummies
    create_monster(&monster_types[2], 9, 3, idx, goblin_counter);
    create_monster(&monster_types[1], 9, 2, idx, mummy_counter);
}

// Helper function for messages
void handle_messages(int fd) {
    message msg{};
    // Read the entire message header (including type)
    ssize_t header_bytes = recv(fd,((char*) &msg) + 1, sizeof(msg) - 1, MSG_WAITALL);
    msg.type = 1;
    if (header_bytes <= 0) {
        printf("Failed to read message struct from client.\n");
        return;
    }

    // Allocate space for the text and read it
    char* text = (char*)malloc(msg.message_length + 2);
    if (!text) {
        printf("Failed to allocate memory for message text.\n");
        return;
    }

    ssize_t mess = recv(fd, text, msg.message_length, MSG_WAITALL);
    if (mess <= 0) {
        printf("Failed to read message text.\n");
        free(text);
        return;
    }
    text[msg.message_length] = '\0';

    // Find the sender
    pthread_mutex_lock(&players_mutex);
    our_player* sender = nullptr;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i] && players[i]->fd == fd) {
            sender = players[i];
            break;
        }
    }
    pthread_mutex_unlock(&players_mutex);

    if (!sender) {
        printf("Unknown sender for fd %d\n", fd);
        free(text);
        return;
    }

    printf("[%s in room %d]: %s\n", sender->name, sender->room, text);

    // Determine if broadcast or private message
    bool is_global = (msg.recipient[0] == '\0' || strcmp(msg.recipient, "ALL") == 0);

    if (is_global) {
        Rooms* room = &game_rooms[sender->room - 1];
        pthread_mutex_lock(&room->lock);
        for (int i = 0; i < room->occupant_count; i++) {
            our_player* occupant = room->occupants[i];
            if (occupant && occupant->fd != fd) {
                write(occupant->fd, &msg, sizeof(msg));
                write(occupant->fd, text, msg.message_length);
            }
        }
        pthread_mutex_unlock(&room->lock);
    } else {
        pthread_mutex_lock(&players_mutex);
        our_player* recipient = nullptr;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i] && strcmp(players[i]->name, msg.recipient) == 0) {
                recipient = players[i];
                break;
            }
        }
        pthread_mutex_unlock(&players_mutex);

        if (recipient) {
            write(recipient->fd, &msg, sizeof(msg));
            write(recipient->fd, text, msg.message_length);
        } else {
            printf("Recipient '%s' not found for message from %s.\n",
                msg.recipient, sender->name);
        }
    }

    free(text);
}

// Fighting helper function
void handle_fight(int fd) {
    // Find the player
    our_player* player = nullptr;
    pthread_mutex_lock(&players_mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i] && players[i]->fd == fd) {
            player = players[i];
            break;
        }
    }
    pthread_mutex_unlock(&players_mutex);

    if (!player) {
        printf("Unknown player for fd %d\n", fd);
        return;
    }

    // Get room
    Rooms* room = &game_rooms[player->room - 1];
    pthread_mutex_lock(&room->lock);

    // Main fight loop
    while (player->health > 0) {
        // Gather alive monsters
        our_player* alive_monsters[MAX_OCCUPANTS];
        int alive_count = 0;
        for (int i = 0; i < room->occupant_count; i++) {
            our_player* occupant = room->occupants[i];
            if ((occupant->flags & FLAG_MONSTER) && (occupant->flags & FLAG_ALIVE) && occupant != player) {
                alive_monsters[alive_count++] = occupant;
            }
        }

        // End fight if no monsters left
        if (alive_count == 0) {
            break;
        }

        char combat_log[512];  // larger buffer for narration

        // Each monster takes a turn
        for (int i = 0; i < alive_count; i++) {
            our_player* monster = alive_monsters[i];

            // Player attacks monster
            int damage_to_monster = player->attack - monster->defence;
            if (damage_to_monster < 1) damage_to_monster = 1;
            monster->health -= damage_to_monster;
            if (monster->health < 0) monster->health = 0;

            // Monster attacks player if alive
            int damage_to_player = 0;
            if (monster->health > 0) {
                damage_to_player = monster->attack - player->defence;
                if (damage_to_player < 1) damage_to_player = 1;
                player->health -= damage_to_player;
                if (player->health < 0) player->health = 0;
            }

            // Prepare narration
            snprintf(combat_log, sizeof(combat_log),
                     "%s hits %s for %d damage. %s health: %d. "
                     "%s hits back for %d damage. %s health: %d.\n",
                     player->name, monster->name, damage_to_monster, monster->name, monster->health,
                     monster->name, damage_to_player, player->name, player->health);

            // Broadcast to human players in room
            for (int j = 0; j < room->occupant_count; j++) {
                our_player* occupant = room->occupants[j];
                if (occupant && occupant->fd > 0) {
                    message msg{};
                    msg.type = 1;
                    msg.message_length = strlen(combat_log);
                    strncpy(msg.sender, "Narrator", sizeof(msg.sender) - 1);
                    msg.sender[sizeof(msg.sender) - 1] = '\0';
                    msg.end_marker[0] = 0;
                    msg.end_marker[1] = 1;
                    write(occupant->fd, &msg, sizeof(msg));
                    write(occupant->fd, combat_log, msg.message_length);
                }
            }

            // Mark dead monsters
            if (monster->health <= 0) {
                monster->flags &= ~FLAG_ALIVE;
                printf("Monster %s has died!\n", monster->name);
            }

            // Stop if player died
            if (player->health <= 0) break;
        }

        if (player->health <= 0) break;
    }

    // Handle fight result
    if (player->health <= 0) {
        printf("Player %s has been killed in room %d!\n", player->name, room->room_number);
        player->flags &= ~FLAG_ALIVE;
    } else {
        printf("Player %s defeated all monsters in room %d!\n", player->name, room->room_number);
    }

    // Remove dead monsters from room occupants
    int write_index = 0;
    for (int i = 0; i < room->occupant_count; i++) {
        our_player* occupant = room->occupants[i];
        if (!(occupant->flags & FLAG_MONSTER) || (occupant->flags & FLAG_ALIVE)) {
            room->occupants[write_index++] = occupant;  // keep alive
        } else {
            printf("Removed dead monster %s from room %d\n", occupant->name, room->room_number);
        }
    }
    room->occupant_count = write_index;

    pthread_mutex_unlock(&room->lock);
}

// PVP Handling
void handle_pvp_fight(int fd) {
    PVP_fight pvp_msg{};
    ssize_t bytes_received = recv(fd, ((char*)&pvp_msg) + 1, sizeof(pvp_msg) - 1, MSG_WAITALL);
    if (bytes_received <= 0) {
        printf("Failed to read PVP struct from client.\n");
        return;
    }

    our_player* sender = nullptr;
    pthread_mutex_lock(&players_mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i] && players[i]->fd == fd) {
            sender = players[i];
            break;
        }
    }
    pthread_mutex_unlock(&players_mutex);

    if (!sender) {
        printf("Unknown sender for fd %d\n", fd);
        return;
    }

    our_player* target = nullptr;
    Rooms* room = &game_rooms[sender->room - 1];
    pthread_mutex_lock(&room->lock);
    for (int i = 0; i < room->occupant_count; i++) {
        our_player* occupant = room->occupants[i];
        if (!(occupant->flags & FLAG_MONSTER) && strcmp(occupant->name, pvp_msg.target) == 0) {
            target = occupant;
            break;
        }
    }
    pthread_mutex_unlock(&room->lock);

    char narration[256];
    if (target) {
        snprintf(narration, sizeof(narration),
                 "Alas the gods shined their light through to you, %s. "
                 "You are only but one trapped in this cave. No need to harm another human soul. "
                 "You have changed your mind and will not fight today.\n",
                 sender->name);
    } else {
        snprintf(narration, sizeof(narration),
                 "OH you silly goooosee %s, you need to use FIGHT for that nonsense [please press 3]\n",
                 sender->name);
    }

    message msg{};
    msg.type = 1;
    msg.message_length = strlen(narration);
    strncpy(msg.sender, "Narrator", sizeof(msg.sender) - 1);
    msg.sender[sizeof(msg.sender) - 1] = '\0';
    msg.end_marker[0] = 0;
    msg.end_marker[1] = 1;

    write(fd, &msg, sizeof(msg));
    write(fd, narration, msg.message_length);
}

// Looting
void handle_loot(int fd) {
    loot loot_msg{};
    ssize_t bytes_received = recv(fd, ((char*)&loot_msg) + 1, sizeof(loot_msg) - 1, MSG_WAITALL);
    if (bytes_received <= 0) {
        printf("Failed to read LOOT struct from client.\n");
        return;
    }

    our_player* sender = nullptr;
    pthread_mutex_lock(&players_mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i] && players[i]->fd == fd) {
            sender = players[i];
            break;
        }
    }
    pthread_mutex_unlock(&players_mutex);

    if (!sender) {
        printf("Unknown sender for fd %d\n", fd);
        return;
    }

    Rooms* room = &game_rooms[sender->room - 1];
    pthread_mutex_lock(&room->lock);

    our_player* target = nullptr;
    for (int i = 0; i < room->occupant_count; i++) {
        our_player* occupant = room->occupants[i];
        if (strcmp(occupant->name, loot_msg.target) == 0) {
            target = occupant;
            break;
        }
    }

    char narration[256];

    if (!target) {
        snprintf(narration, sizeof(narration),
                 "There is no one by the name '%s' here to loot.\n", loot_msg.target);
    } else if (target->flags & FLAG_ALIVE) {
        snprintf(narration, sizeof(narration),
                 "%s is still alive! You cannot loot them.\n", target->name);
    } else if (target->gold == 0) {
        snprintf(narration, sizeof(narration),
                 "%s has no gold to loot.\n", target->name);
    } else {
        uint16_t gold_looted = target->gold;
        sender->gold += gold_looted;
        target->gold = 0;
        snprintf(narration, sizeof(narration),
                 "You successfully looted %d gold from %s.\n", gold_looted, target->name);
    }

    pthread_mutex_unlock(&room->lock);

    message msg{};
    msg.type = 1;
    msg.message_length = strlen(narration);
    strncpy(msg.sender, "Narrator", sizeof(msg.sender) - 1);
    msg.sender[sizeof(msg.sender) - 1] = '\0';
    msg.end_marker[0] = 0;
    msg.end_marker[1] = 1;

    write(fd, &msg, sizeof(msg));
    write(fd, narration, msg.message_length);
}

// Initialize rooms
void init_rooms() {
    const char* room_names[MAX_ROOMS] = {
        "Cave Entrance", "Cave Waterfall", "Crystal Cavern", "Goblin Nest",
        "Mushroom Grove", "Underground River", "Collapsed Tunnel",
        "Ancient Shrine", "Treasure Vault", "Climbing Rope"
    };

    const char* room_descs[MAX_ROOMS] = {
        "You have fallen into a dark cave. Mystery and a cool breeze surrounds you. "
        "Fight your way out if you must. Find the rope and climb your way out!",
        "*RRRSHSSHHHSHSHHH WOOOSHHERHHSHSHSHSH* You hear the sounds of a roaring "
        "waterfall cascading into a pool. Mist fills the air. Danger lurks...",
        "The walls glitter with faintly glowing crystals. Maybe you can profit a little... ;)",
        "Growls and evil cackles fill the room... Oh no.. They smell you... "
        "You entered the goblin cave.. RUNN!!!",
        "Bioluminescent mushrooms light the path blue... eerie...",
        "A river cuts through the tunnel, blocking your path. You can barely see the other side.",
        "Rubble blocks part of the tunnel. There must be a way to squeeze through here...",
        "HOLY CONOLY!!! JACKPOT... just kidding... but you follow the strange carvings "
        "glowing faintly. They lead you to an ancient Shrine to the Goblin Gods.",
        "Gold and jewels sparkle in the torchlight... dangers may lurk very close, keep a sharp eye.",
        "You see faint daylight... You get closer to see a rope hanging from the ceiling — freedom lies ahead!"
    };

    // Loop for initialization
    for (int i = 0; i < MAX_ROOMS; i++) {
        game_rooms[i].room_number = i + 1;
        strncpy(game_rooms[i].name, room_names[i], sizeof(game_rooms[i].name) - 1);
        game_rooms[i].name[sizeof(game_rooms[i].name) - 1] = '\0';
        game_rooms[i].room_description = strdup(room_descs[i]);
        game_rooms[i].occupant_count = 0;
        pthread_mutex_init(&game_rooms[i].lock, NULL);
    }

    // Hardcode room connections
    // Cave Entrance (1)
    game_rooms[0].num_connections = 2;
    game_rooms[0].connections[0] = {2, "Cave Waterfall", strdup("You hear rushing water nearby.")};
    game_rooms[0].connections[1] = {3, "Crystal Cavern", strdup("The faint glow of crystals beckons.")};

    // Cave Waterfall (2)
    game_rooms[1].num_connections = 2;
    game_rooms[1].connections[0] = {1, "Cave Entrance", strdup("The way back to where you fell in.")};
    game_rooms[1].connections[1] = {5, "Mushroom Grove", strdup("Soft blue light glows deeper in.")};

    // Crystal Cavern (3)
    game_rooms[2].num_connections = 3;
    game_rooms[2].connections[0] = {1, "Cave Entrance", strdup("The dark tunnel leads back.")};
    game_rooms[2].connections[1] = {4, "Goblin Nest", strdup("You hear cackling up ahead...")};
    game_rooms[2].connections[2] = {6, "Underground River", strdup("You feel cool air flowing through.")};

    // Goblin Nest (4)
    game_rooms[3].num_connections = 2;
    game_rooms[3].connections[0] = {3, "Crystal Cavern", strdup("Crystals glint faintly behind you.")};
    game_rooms[3].connections[1] = {7, "Collapsed Tunnel", strdup("The path is half-blocked by rubble.")};

    // Mushroom Grove (5)
    game_rooms[4].num_connections = 2;
    game_rooms[4].connections[0] = {2, "Cave Waterfall", strdup("The roar of the falls fades behind you.")};
    game_rooms[4].connections[1] = {6, "Underground River", strdup("A rushing sound comes from the next tunnel.")};

    // Underground River (6)
    game_rooms[5].num_connections = 3;
    game_rooms[5].connections[0] = {3, "Crystal Cavern", strdup("Glittering reflections lead back that way.")};
    game_rooms[5].connections[1] = {5, "Mushroom Grove", strdup("The glowing mushrooms fade behind you.")};
    game_rooms[5].connections[2] = {8, "Ancient Shrine", strdup("A glowing path leads toward strange carvings.")};

    // Collapsed Tunnel (7)
    game_rooms[6].num_connections = 2;
    game_rooms[6].connections[0] = {4, "Goblin Nest", strdup("You hear faint goblin laughter echoing back.")};
    game_rooms[6].connections[1] = {8, "Ancient Shrine", strdup("Symbols glow faintly through the dust.")};

    // Ancient Shrine (8)
    game_rooms[7].num_connections = 3;
    game_rooms[7].connections[0] = {6, "Underground River", strdup("A faint mist lingers from the waterway.")};
    game_rooms[7].connections[1] = {7, "Collapsed Tunnel", strdup("The rubble behind you seems unstable.")};
    game_rooms[7].connections[2] = {9, "Treasure Vault", strdup("Golden light spills from ahead...")};

    // Treasure Vault (9)
    game_rooms[8].num_connections = 2;
    game_rooms[8].connections[0] = {8, "Ancient Shrine", strdup("The shrine's eerie glow fades behind.")};
    game_rooms[8].connections[1] = {10, "Climbing Rope", strdup("You see faint daylight above!")};

    // Climbing Rope (10)
    game_rooms[9].num_connections = 1;
    game_rooms[9].connections[0] = {9, "Treasure Vault", strdup("Treasure glints faintly below.")};

    printf("Rooms initialized (%d total).\n", MAX_ROOMS);
}

// Handle changing rooms
void handle_change_room(int fd) {
    change_room CHANGErooms;
    ssize_t n = recv(fd, ((char*)&CHANGErooms) + 1, sizeof(CHANGErooms) - 1, MSG_WAITALL);
    CHANGErooms.type = 2;

    if(n <= 0){
        printf("Failed to read change_room\n");
        send_error(fd, 0, "Failed to change room. Please find another path to take.");
        return;
    }
    CHANGErooms.type = 2;
    
    //find the player
    pthread_mutex_lock(&players_mutex);
    our_player* player = nullptr;
    for(int i = 0; i < MAX_PLAYERS; i++){
        if(players[i] && players[i]->fd == fd){
            player = players[i];
            break;
        }
    }
    pthread_mutex_unlock(&players_mutex);

    if(!player){
        send_error(fd, 5, "You are not ready yet. Please gather your gear and try again");
        return;
    }
    printf("Received CHANGEROOM request: %s -> room %d\n", player->name, CHANGErooms.room_number);

    //Validation of room number
    if(CHANGErooms.room_number < 1 || CHANGErooms.room_number > MAX_ROOMS){
        send_error(fd, 1, "Invalid room. THis is not part of the path.");
        return;
    }

    Rooms* old_room = &game_rooms[player->room - 1];
    bool valid = false;
    for(int i = 0; i < old_room->num_connections; i++){
        if(old_room->connections[i].room_number == CHANGErooms.room_number){
            valid = true;
            break;
        }
    }
    if(!valid){
        send_error(fd, 1, "Invalid room. This path is different, but not allowed here. Try again");
        return;
    }

    //move the player
    Rooms* new_room = &game_rooms[CHANGErooms.room_number - 1];

    pthread_mutex_lock(&old_room->lock);
    for (int i = 0; i < old_room->occupant_count; i++) {
        if (old_room->occupants[i] == player) {
            old_room->occupants[i] = old_room->occupants[old_room->occupant_count - 1];
            old_room->occupant_count--;
            break;
        }
    }
    pthread_mutex_unlock(&old_room->lock);

    pthread_mutex_lock(&new_room->lock);
    if (new_room->occupant_count < MAX_OCCUPANTS) {
        new_room->occupants[new_room->occupant_count++] = player;
        player->room = CHANGErooms.room_number;
    } else {
        pthread_mutex_unlock(&new_room->lock);
        send_error(fd, 0, "Room is full. Move along.");
        return;
    }
    pthread_mutex_unlock(&new_room->lock);

    printf("Player %s moved to room %d: %s\n", player->name, new_room->room_number, new_room->name);

    //send the room info
    room_message messageRoom;
    messageRoom.type = 9;
    messageRoom.room_number = new_room->room_number;
    strncpy(messageRoom.room_name, new_room->name, sizeof(messageRoom.room_name) - 1);
    messageRoom.room_name[sizeof(messageRoom.room_name) - 1] = '\0';
    messageRoom.description_length = strlen(new_room->room_description);
    write(fd, &messageRoom, sizeof(messageRoom));
    write(fd, new_room->room_description, messageRoom.description_length);

    //send the connection message
    for(int i = 0; i < new_room->num_connections; i++){
        if(new_room->connections[i].room_number ==0){
            continue;
        }
        Connection* connection = &new_room->connections[i];
        connection_message connection_msg;
        connection_msg.type = 13;
        connection_msg.room_number = connection->room_number;
        strncpy(connection_msg.room_name, connection->name, sizeof(connection_msg.room_name) - 1);
        connection_msg.room_name[sizeof(connection_msg.room_name) - 1] = '\0';
        connection_msg.description_length = strlen(connection->desc);
        write(fd, &connection_msg, sizeof(connection_msg));
        write(fd, connection->desc, connection_msg.description_length);
    }
    printf("Sent %d connection(s) for room %d (%s)\n",
           new_room->num_connections, new_room->room_number, new_room->name);

    // Send all other occupants + monsters currently in the new room
    pthread_mutex_lock(&new_room->lock);
    for (int i = 0; i < new_room->occupant_count; i++) {
        our_player* occ = new_room->occupants[i];
        if (!occ || occ == player) continue;  // skip self

        const char* desc = nullptr;

        if (occ->flags & FLAG_MONSTER) {
            // find monster description
            for (int m = 0; m < MAX_MONSTERS; m++) {
                if (&monsters[m] == occ) {
                    desc = monster_descs[m];
                    break;
                }
            }
        } else {
            // find player description
            int occ_slot = -1;
            pthread_mutex_lock(&players_mutex);
            for (int j = 0; j < MAX_PLAYERS; j++) {
                if (players[j] == occ) { occ_slot = j; break; }
            }
            pthread_mutex_unlock(&players_mutex);
            if (occ_slot != -1) desc = player_descs[occ_slot];
        }

        send_character_snapshot(fd, occ, desc);
    }
    pthread_mutex_unlock(&new_room->lock);

    
    //broadcast to room
    pthread_mutex_lock(&new_room->lock);
    for (int i = 0; i < new_room->occupant_count; i++) {
        our_player* occ = new_room->occupants[i];
        if (occ && occ->fd > 0 && !(occ->flags & FLAG_MONSTER) && occ->fd != fd) {
            char enter_msg[128];
            snprintf(enter_msg, sizeof(enter_msg), "%s has entered %s.\n", player->name, new_room->name);

            message m{};
            m.type = 1;
            m.message_length = strlen(enter_msg);
            strncpy(m.sender, "Narrator", sizeof(m.sender) - 1);
            m.sender[sizeof(m.sender) - 1] = '\0';
            m.end_marker[0] = 0;
            m.end_marker[1] = 1;

            write(occ->fd, &m, sizeof(m));
            write(occ->fd, enter_msg, m.message_length);
        }
    }
    pthread_mutex_unlock(&new_room->lock);

    // Send updated CHARACTER to confirm move
    int slot = -1;
    pthread_mutex_lock(&players_mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i] == player) {
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&players_mutex);

    if (slot != -1) {
        send_character_snapshot(fd, player, player_descs[slot]);
    }

    //move room narration
    char move_msg_text[128];
    snprintf(move_msg_text, sizeof(move_msg_text),
            "You moved to %s.", new_room->name);

    message move_msg{};
    move_msg.type = 1;
    move_msg.message_length = strlen(move_msg_text);
    strncpy(move_msg.sender, "Narrator", sizeof(move_msg.sender) - 1);
    move_msg.sender[sizeof(move_msg.sender) - 1] = '\0';
    move_msg.end_marker[0] = 0;
    move_msg.end_marker[1] = 1;
    write(fd, &move_msg, sizeof(move_msg));
    write(fd, move_msg_text, move_msg.message_length);

}

// Remove a player from their current room
void remove_player_from_room(our_player* p) {
    if (!p || p->room < 1 || p->room > MAX_ROOMS) {
        return;
    }

    Rooms* room = &game_rooms[p->room - 1];
    pthread_mutex_lock(&room->lock);
    for (int i = 0; i < room->occupant_count; i++) {
        if (room->occupants[i] == p) {
            room->occupants[i] = room->occupants[room->occupant_count - 1];
            room->occupant_count--;
            printf("Player %s removed from room %d (%s)\n",
                   p->name, room->room_number, room->name);
            break;
        }
    }
    pthread_mutex_unlock(&room->lock);
}

// Remove just the disconnected player helper
void remove_player(int index) {
    if (player_descs[index]) {
        free((void*)player_descs[index]);
        player_descs[index] = nullptr;
    }

    if (players[index]) {
        free(players[index]);
        players[index] = nullptr;
    }
}

//Handle leave
void handle_leave(int fd){
    pthread_mutex_lock(&players_mutex);
    our_player* player = nullptr;
    int index = -1;

    for(int i = 0; i < MAX_PLAYERS; i++){
        if(players[i] && players[i]->fd == fd){
            player = players[i];
            index = i;
            break;
        }
    }
    pthread_mutex_unlock(&players_mutex);

    if(!player){
        return;
    }

    // Announcement to others in the room
    Rooms* room = &game_rooms[player->room - 1];
    char leave_msg[128];
    snprintf(leave_msg, sizeof(leave_msg), "%s has left the room.", player->name);

    message msg{};
    msg.type = 1;
    msg.message_length = strlen(leave_msg);
    strncpy(msg.sender, "Narrator", sizeof(msg.sender) - 1);
    msg.sender[sizeof(msg.sender) - 1] = '\0';
    msg.end_marker[0] = 0;
    msg.end_marker[1] = 1;

    pthread_mutex_lock(&room->lock);
    for (int i = 0; i < room->occupant_count; i++) {
        our_player* occupant = room->occupants[i];
        if (occupant && occupant->fd != fd) {
            write(occupant->fd, &msg, sizeof(msg));
            write(occupant->fd, leave_msg, msg.message_length);
        }
    }

    // Remove from occupants
    for (int i = 0; i < room->occupant_count; i++) {
        if (room->occupants[i] == player) {
            room->occupants[i] = room->occupants[room->occupant_count - 1];
            room->occupant_count--;
            break;
        }
    }
    pthread_mutex_unlock(&room->lock);

    printf("Player %s has left room %d (%s)\n", player->name, room->room_number, room->name);

    // Clean up player memory
    pthread_mutex_lock(&players_mutex);
    players[index] = nullptr;
    pthread_mutex_unlock(&players_mutex);

    close(fd);
}

//snapshot helper for sending info
void send_character_snapshot(int fd, our_player* occupant, const char* desc) {
    if (!occupant){
        return;
    }

    our_player temp = *occupant;
    temp.fd = 0;               
    temp.type = 10;
    // Send the base CHARACTER struct
    ssize_t bytes = write(fd, &temp, sizeof(temp) - sizeof(int));
    if (bytes != ssize_t(sizeof(temp) - sizeof(int))) {
        perror("send_character_snapshot: write failed");
        return;
    }

    // Send description if any
    if (desc && temp.description_length > 0) {
        write(fd, desc, temp.description_length);
    }
}

// --- CLIENT HANDLING ---
void* client_thread(void* arg) {
    int fd = *(int*)arg;
    delete (int*)arg;  // free memory

    // --- Send VERSION message ---
    version_message v;
    write(fd, &v, sizeof(v));

    // --- Send GAME message ---
    const char* game_desc =
        "You are an Adventurer wandering through the woods..... "
        "AHHHH OH NO!! You fell into a cave!\n"
        "Avoid the mummies, bats, and goblins!\n"
        "Find a way out and get back to your family!!";

    game_message g;
    g.type = 11;
    g.initial_points = 150;
    g.stat_limit = 200;
    g.description_length = strlen(game_desc);

    // Send struct
    if (write(fd, &g, sizeof(g)) != sizeof(g)) {
        perror("Failed to send game_message struct");
    }

    // Send description
    if (write(fd, game_desc, g.description_length) != g.description_length) {
        perror("Failed to send game description");
    }

    // --- Wait for CHARACTER creation ---
    our_player player = {};

    for (;;) {
        uint8_t type;
        ssize_t n = recv(fd, &type, 1, MSG_WAITALL);
        if (n <= 0) {
            printf("Client disconnected.\n");
            break;
        }

        if (type != 10) {
            printf("Waiting for CHARACTER, got type %d\n", type);
            continue;
        }

        player.type = type;

        // Read the rest of the CHARACTER struct (excluding fd)
        ssize_t structBytes_received = recv(fd, ((char*)&player) + 1, sizeof(our_player) - 1 - sizeof(player.fd), MSG_WAITALL);
        if (structBytes_received <= 0) {
            printf("Failed to read CHARACTER data.\n");
            return nullptr;
        }

        // Read the description if present
        char* desc = nullptr;
        if (player.description_length > 0) {
            desc = (char*)malloc(player.description_length + 1);
            if (!desc) {
                printf("Failed to allocate description memory.\n");
                return nullptr;
            }

            ssize_t desc_bytes = recv(fd, desc, player.description_length, MSG_WAITALL);
            if (desc_bytes <= 0) {
                printf("Failed to read CHARACTER description.\n");
                free(desc);
                return nullptr;
            }
            desc[player.description_length] = '\0';
        }

        printf("Received Character: %s atk=%d def=%d reg=%d desc_len=%d\n",
            player.name, player.attack, player.defence, player.regen, player.description_length);

        // --- Stat check ---
        int total = player.attack + player.defence + player.regen;
        printf("start stat check...%d %d %d\n", player.attack, player.defence, player.regen);

        if (total > g.initial_points) {
            send_error(fd, 4, "Stats too high! Try again!");

            if (desc) {
                free(desc);
                desc = nullptr;
            }
            continue;
        }

        // --- Accept CHARACTER ---
        pthread_mutex_lock(&players_mutex);
        int slot = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i] == NULL) {
                slot = i;
                break;
            }
        }

        if (slot != -1) {
            players[slot] = (our_player*)malloc(sizeof(our_player));
            *players[slot] = player;
            players[slot]->fd = fd;
            players[slot]->room = 1;
            players[slot]->flags = FLAG_ALIVE | FLAG_STARTED;
            players[slot]->gold = 0;
            if (players[slot]->health <= 0) {
                players[slot]->health = 100;
            }

            if (player.description_length > 0) {
                player_descs[slot] = strdup(desc);
            } else {
                player_descs[slot] = nullptr;
            }
        }
        pthread_mutex_unlock(&players_mutex);

        // Send ACCEPT
        accept_message character_accept;
        character_accept.type = 8;
        character_accept.accepted_type = 10;
        ssize_t bytes_sent = send(fd, &character_accept, sizeof(character_accept), MSG_NOSIGNAL);
        if (bytes_sent != sizeof(character_accept)) {
            perror("send accept_message failed");
        }

        printf("Sent ACCEPT message (0x%02X)\n", character_accept.type);
        printf("Accepted character %s\n", player.name);

        if (desc) {
            free(desc);
            desc = nullptr;
        }
        break;
    }


    // --- Wait for START ---
    for (;;) {
        uint8_t type;
        int n = read(fd, &type, 1);
        if (n <= 0) {
            return nullptr;
        }
        if (type == 6) break;
    }

    // --- Player struct ---
    our_player* p = nullptr;
    int p_slot = -1;

    pthread_mutex_lock(&players_mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i] && players[i]->fd == fd) {
            p = players[i];
            p_slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&players_mutex);

    if (!p) {
        printf("Error: could not find player for fd %d in START phase.\n", fd);
        close(fd);
        return nullptr;
    }

    // Send the player's CHARACTER snapshot
    send_character_snapshot(fd, p, player_descs[p_slot]);

    // Add player to starting room
    Rooms* spawn_room = &game_rooms[p->room - 1];
    pthread_mutex_lock(&spawn_room->lock);
    if (spawn_room->occupant_count < MAX_OCCUPANTS) {
        spawn_room->occupants[spawn_room->occupant_count++] = p;
        printf("Player %s entered room %d: %s\n",
               p->name, spawn_room->room_number, spawn_room->name);
    }
    pthread_mutex_unlock(&spawn_room->lock);

    // Send ACCEPT for START (type 6)
    accept_message start_accept;
    start_accept.type = 8;
    start_accept.accepted_type = 6;
    write(fd, &start_accept, sizeof(start_accept));

    // Send initial room 
    Rooms* room = spawn_room;
    room_message r;
    r.type = 9;
    r.room_number = room->room_number;
    strncpy(r.room_name, room->name, sizeof(r.room_name) - 1);
    r.room_name[sizeof(r.room_name) - 1] = '\0';
    r.description_length = strlen(room->room_description);
    write(fd, &r, sizeof(r));
    write(fd, room->room_description, r.description_length);

    // Send all connections for this room
    for (int i = 0; i < room->num_connections; i++) {
        Connection* conn = &room->connections[i];
        connection_message conn_msg;
        conn_msg.type = 13;
        conn_msg.room_number = conn->room_number;
        strncpy(conn_msg.room_name, conn->name, sizeof(conn_msg.room_name) - 1);
        conn_msg.room_name[sizeof(conn_msg.room_name) - 1] = '\0';
        conn_msg.description_length = strlen(conn->desc);

        write(fd, &conn_msg, sizeof(conn_msg));
        write(fd, conn->desc, conn_msg.description_length);
    }
    printf("Sent %d connection(s) for room %d (%s)\n",
           room->num_connections, room->room_number, room->name);

    // Send all other occupants + monsters in this room
    pthread_mutex_lock(&room->lock);
    for (int i = 0; i < room->occupant_count; i++) {
        our_player* occupant = room->occupants[i];
        if (!occupant || occupant == p) continue;

        const char* desc = nullptr;

        if (occupant->flags & FLAG_MONSTER) {
            // Monster lookup
            for (int m = 0; m < MAX_MONSTERS; m++) {
                if (&monsters[m] == occupant) {
                    desc = monster_descs[m];
                    break;
                }
            }
        } else {
            // Player lookup
            int occ_slot = -1;
            pthread_mutex_lock(&players_mutex);
            for (int j = 0; j < MAX_PLAYERS; j++) {
                if (players[j] == occupant) { occ_slot = j; break; }
            }
            pthread_mutex_unlock(&players_mutex);
            if (occ_slot != -1)
                desc = player_descs[occ_slot];
        }

        // Send snapshot if it's not the current player
        if ((occupant->flags & FLAG_MONSTER) ||
            (occupant->fd > 0 && occupant->fd != fd)) {
            send_character_snapshot(fd, occupant, desc);
        }
    }
    pthread_mutex_unlock(&room->lock);

    printf("Finished sending initial room + occupants snapshot.\n");


    // --- IN-GAME LOOP ---
    while (true) {
        uint8_t type;
        int n = read(fd, &type, 1);
        if (n <= 0) {
            printf("Client disconnected.\n");
            break;
        }

        switch (type) {
            case 1:
                handle_messages(fd);
                break;
            case 2:
                handle_change_room(fd);
                break;
            case 3:
                handle_fight(fd);
                break;
            case 4:
                handle_pvp_fight(fd);
                break;
            case 5:
                handle_loot(fd);
                break;
            case 12:
                handle_leave(fd);
                return nullptr;
            
            default:
                printf("Unknown message type %d from client\n", type);
                break;
        }
    }
    // --- Cleanup ---
    pthread_mutex_lock(&players_mutex);
    int remove_index = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i] && players[i]->fd == fd) {
            p = players[i];
            remove_index = i;
            break;
        }
    }
    pthread_mutex_unlock(&players_mutex);

    // Remove from room
    if (p) {
        remove_player_from_room(p);
    }

    // Free player slot and description
    if (remove_index != -1) {
        pthread_mutex_lock(&players_mutex);
        remove_player(remove_index);
        pthread_mutex_unlock(&players_mutex);
    }

    close(fd);
    return nullptr;
}

// --- MAIN SERVER ---
int main() {
    srand(time(NULL));

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5018);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    init_rooms();
    init_monsters();

    printf("Server running on port 5018...\n");

    // Accept loop
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int* client_fd = new int;
        *client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (*client_fd < 0) {
            perror("accept");
            delete client_fd;
            continue;
        }

        pthread_t t;
        pthread_create(&t, nullptr, client_thread, client_fd);
        pthread_detach(t);
    }

    // Cleanup (only reached on shutdown)
    for (int i = 0; i < MAX_MONSTERS; i++) {
        if (monster_descs[i]) {
            free((void*)monster_descs[i]);
            monster_descs[i] = nullptr;
        }
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_descs[i]) {
            free((void*)player_descs[i]);
            player_descs[i] = nullptr;
        }
        if (players[i]) {
            free(players[i]);
            players[i] = nullptr;
        }
    }

    close(server_fd);
    return 0;
}
