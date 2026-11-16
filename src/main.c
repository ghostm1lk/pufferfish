#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pufferfish.h"

#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

// counts every position reachable in exactly `depth` moves.
// if this matches the known numbers, move generation is correct
static U64 perft(const Board *b, int depth) {
    if (depth == 0) return 1;

    MoveList list;
    generate_moves(b, &list);

    U64 nodes = 0;
    for (int i = 0; i < list.count; i++) {
        Board next = *b;
        if (make_move(&next, list.moves[i])) nodes += perft(&next, depth - 1);
    }
    return nodes;
}

// perft split by first move, same output format as stockfish's "go perft"
// so the two can be diffed when hunting a movegen bug
static void divide(const Board *b, int depth) {
    MoveList list;
    generate_moves(b, &list);

    clock_t start = clock();
    U64 total = 0;
    for (int i = 0; i < list.count; i++) {
        Board next = *b;
        if (!make_move(&next, list.moves[i])) continue;
        U64 nodes = perft(&next, depth - 1);
        char str[6];
        move_to_str(list.moves[i], str);
        printf("%s: %llu\n", str, (unsigned long long)nodes);
        total += nodes;
    }
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;

    printf("\nNodes searched: %llu\n", (unsigned long long)total);
    if (seconds > 0) printf("%.2fs, %.1fM nodes/s\n", seconds, total / seconds / 1e6);
}

// standard perft positions from the chess programming wiki.
// between them they cover castling, en passant, promotions and pins
static int run_tests(void) {
    static const struct { const char *fen; int depth; U64 expected; } tests[] = {
        {START_FEN, 5, 4865609},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487},
        {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594},
    };

    int failed = 0;
    for (int i = 0; i < (int)(sizeof tests / sizeof tests[0]); i++) {
        Board b;
        U64 got = parse_fen(&b, tests[i].fen) ? perft(&b, tests[i].depth) : 0;
        int ok = got == tests[i].expected;
        if (!ok) failed++;
        printf("%s  depth %d  expected %llu  got %llu\n", ok ? "PASS" : "FAIL", tests[i].depth,
               (unsigned long long)tests[i].expected, (unsigned long long)got);
    }
    return failed;
}

int main(int argc, char **argv) {
    init_tables();

    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        return run_tests() ? 1 : 0;
    }

    if (argc >= 3 && strcmp(argv[1], "perft") == 0) {
        Board b;
        int depth = atoi(argv[2]);
        if (depth < 1 || !parse_fen(&b, argc >= 4 ? argv[3] : START_FEN)) {
            fprintf(stderr, "bad depth or fen\n");
            return 1;
        }
        divide(&b, depth);
        return 0;
    }

    fprintf(stderr, "usage: pufferfish test\n       pufferfish perft <depth> [\"fen\"]\n");
    return 1;
}
