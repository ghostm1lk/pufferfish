#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pufferfish.h"

// uci is the text protocol chess guis use to drive an engine:
// the gui writes commands to our stdin and reads replies from our stdout

// finds the legal move matching a string like "e2e4". returns 0 if there is none
static Move parse_move(const Board *b, const char *str) {
    MoveList list;
    generate_moves(b, &list);
    for (int i = 0; i < list.count; i++) {
        char s[6];
        move_to_str(list.moves[i], s);
        if (strcmp(s, str) == 0) return list.moves[i];
    }
    return 0;
}

// position [startpos | fen <fen>] [moves <move> <move> ...]
static void cmd_position(Board *b, char *line) {
    char *moves = strstr(line, " moves ");
    if (moves) {
        *moves = '\0';
        moves += 7;
    }

    char *fen = strstr(line, "fen ");
    if (!parse_fen(b, fen ? fen + 4 : START_FEN)) {
        printf("info string bad fen, using the start position\n");
        parse_fen(b, START_FEN);
    }

    if (!moves) return;
    for (char *tok = strtok(moves, " "); tok; tok = strtok(NULL, " ")) {
        Move m = parse_move(b, tok);
        if (!m || !make_move(b, m)) {
            printf("info string illegal move %s\n", tok);
            return;
        }
    }
}

// reads the number after a parameter name, or -1 if it isn't there
static int go_param(const char *line, const char *name) {
    const char *p = strstr(line, name);
    return p ? atoi(p + strlen(name)) : -1;
}

// go [depth <n>] [movetime <ms>] [wtime <ms> btime <ms> winc <ms> binc <ms>]
static void cmd_go(const Board *b, const char *line) {
    int depth = go_param(line, " depth ");
    int movetime = go_param(line, " movetime ");
    int time_left = go_param(line, b->side == WHITE ? " wtime " : " btime ");
    int inc = go_param(line, b->side == WHITE ? " winc " : " binc ");

    int max_depth = 64;
    int time_ms = 1000;
    if (depth > 0) {
        max_depth = depth;
        time_ms = 1 << 30;
    } else if (movetime > 0) {
        time_ms = movetime - 50;
    } else if (time_left > 0) {
        // spend about a thirtieth of the clock per move, never more than half of it
        time_ms = time_left / 30 + (inc > 0 ? inc / 2 : 0);
        if (time_ms > time_left / 2) time_ms = time_left / 2;
    }
    if (time_ms < 10) time_ms = 10;

    char str[6] = "0000";
    Move best = search(b, max_depth, time_ms);
    if (best) move_to_str(best, str);
    printf("bestmove %s\n", str);
}

// ponytail: the search blocks this loop, so "stop" and "go infinite" don't work.
// games don't need them. analysis mode would, and that means a search thread
void uci_loop(void) {
    // guis read our output through a pipe, so it must not sit in a buffer
    setvbuf(stdout, NULL, _IONBF, 0);

    Board b;
    parse_fen(&b, START_FEN);

    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "uci") == 0) {
            printf("id name Pufferfish\nid author Waleed Abdellatif\nuciok\n");
        } else if (strcmp(line, "isready") == 0) {
            printf("readyok\n");
        } else if (strncmp(line, "position", 8) == 0) {
            cmd_position(&b, line);
        } else if (strncmp(line, "go", 2) == 0) {
            cmd_go(&b, line);
        } else if (strcmp(line, "quit") == 0) {
            break;
        }
    }
}
