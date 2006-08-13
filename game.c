/*
   
    Copyright (c) 2006 Florian Wesch <fw@dividuum.de>. All Rights Reserved.
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

*/

#include <sys/time.h>
#include <time.h>

#include <lauxlib.h>

#include "scroller.h"
#include "world.h"
#include "global.h"
#include "player.h"
#include "creature.h"
#include "misc.h"

static int            should_end_round = 0;
static struct timeval start;
static char           intermission[256] = {0};

static int get_tick() {
    static struct timeval now;
    gettimeofday(&now , NULL);
    return (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
}

void game_send_info(client_t *client) {
    packet_t packet;
    packet_init(&packet, PACKET_GAME_INFO);
    packet_write32(&packet, game_time);
    server_send_packet(&packet, client);
}

void game_send_round_info(client_t *client, int delta) {
    packet_t packet;
    packet_init(&packet, PACKET_ROUND);
    packet_write08(&packet, delta);
    server_send_packet(&packet, client);
}

void game_send_intermission(client_t *client) {
    packet_t packet;
    packet_init(&packet, PACKET_INTERMISSION);
    packet_writeXX(&packet, intermission, strlen(intermission));
    server_send_packet(&packet, client);
}

void game_send_initial_update(client_t *client) {
    game_send_info(client);
    game_send_intermission(client);
}

static int luaGameEnd(lua_State *L) {
    should_end_round = 1;
    return 0;
}

static int luaGameInfo(lua_State *L) {
    lua_pushnumber(L, game_time);
    lua_pushnumber(L, world_width());
    lua_pushnumber(L, world_height());
    return 3;
}

static int luaGameTime(lua_State *L) {
    lua_pushnumber(L, game_time);
    return 1;
}

static int luaScrollerAdd(lua_State *L) {
    add_to_scroller(luaL_checkstring(L, 1));
    return 0;
}

static int luaGameIntermission(lua_State *L) {
    snprintf(intermission, sizeof(intermission), "%s", luaL_checkstring(L, 1));
    game_send_intermission(SEND_BROADCAST);
    return 0;
}

void game_init() {
    lua_pushliteral(L, "rules_init");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_pcall(L, 0, 0, 0) != 0) {
        fprintf(stderr, "error calling rules_init: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_register(L, "game_end",         luaGameEnd);
    lua_register(L, "game_info",        luaGameInfo);
    lua_register(L, "game_time",        luaGameTime);

    lua_register(L, "set_intermission", luaGameIntermission);

    lua_register(L, "scroller_add",     luaScrollerAdd);

    lua_pushnumber(L, MAXPLAYERS);
    lua_setglobal(L, "MAXPLAYERS");

    lua_pushliteral(L, GAME_NAME);
    lua_setglobal(L,  "GAME_NAME");
}

void game_one_round() {
    should_end_round = 0;
    game_time        = 0;
    
    game_send_info(SEND_BROADCAST);
    
    lua_pushliteral(L, "onNewRound");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_pcall(L, 0, 0, 0) != 0) {
        fprintf(stderr, "error calling onNewRound: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    world_init();
    creature_init();

    // Initialer Worldtick
    world_tick();

    // Beginn der Zeitrechnung...
    int lasttick = 0;
    gettimeofday(&start, NULL);

    player_round_start();

    while (!game_exit && !should_end_round) {
        int tick  = get_tick();
        int delta = tick - lasttick;

        if (delta < 0 || delta > 200) {
            // Timewarp?
            lasttick = tick;
            continue;
        } else if (delta < 100) {
            usleep(max(95000 - delta * 1000, 1000));
            continue;
        }

        lasttick = tick;

        if (!game_paused) {
            // Runde starten
            game_send_round_info(SEND_BROADCAST, delta);

            // World Zeugs
            world_tick();

            // Spielerprogramme ausfuehren
            player_think();

            // Viecher bewegen
            creature_moveall(delta);

            // Zeit weiterlaufen lassen
            game_time += delta;

            lua_pushliteral(L, "onRound");
            lua_rawget(L, LUA_GLOBALSINDEX);
            if (lua_pcall(L, 0, 0, 0) != 0) {
                fprintf(stderr, "error calling onNewRound: %s\n", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        
        // IO Lesen/Schreiben
        server_tick();
    }

    creature_shutdown();
    world_shutdown();
}