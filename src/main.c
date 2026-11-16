#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pufferfish.h"

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

// walks the move tree checking that the incrementally updated key
// matches one computed from scratch. returns the number of mismatches
static int check_keys(const Board *b, int depth) {
    int bad = b->key != compute_key(b);
    if (depth == 0) return bad;

    MoveList list;
    generate_moves(b, &list);
    for (int i = 0; i < list.count; i++) {
        Board next = *b;
        if (make_move(&next, list.moves[i])) bad += check_keys(&next, depth - 1);
    }
    return bad;
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

    // hash check on two positions that cover castling, en passant and promotions
    Board b;
    char str[6];
    parse_fen(&b, tests[1].fen);
    int bad_keys = check_keys(&b, 3);
    parse_fen(&b, tests[3].fen);
    bad_keys += check_keys(&b, 3);
    if (bad_keys) failed++;
    printf("%s  hash keys  %d mismatches\n", bad_keys ? "FAIL" : "PASS", bad_keys);

    // search check: the only mate in one here is Ra8
    parse_fen(&b, "6k1/5ppp/8/8/8/8/8/R3K3 w - - 0 1");
    move_to_str(search(&b, 2, 1000), str);
    int ok = strcmp(str, "a1a8") == 0;
    if (!ok) failed++;
    printf("%s  mate in one  expected a1a8  got %s\n", ok ? "PASS" : "FAIL", str);

    // repetition check: black is a queen down, and after Ka2 Kh7 Ka1 the move
    // Kh8 brings back the first position. a draw beats being lost, so play it
    parse_fen(&b, "7k/8/8/8/8/8/8/KQ6 w - - 0 1");
    static const Move shuffle[3] = {MOVE(0, 8, QUIET), MOVE(63, 55, QUIET), MOVE(8, 0, QUIET)};
    history_len = 0;
    for (int i = 0; i < 3; i++) {
        history[history_len++] = b.key;
        make_move(&b, shuffle[i]);
    }
    move_to_str(search(&b, 2, 1000), str);
    history_len = 0;
    ok = strcmp(str, "h7h8") == 0;
    if (!ok) failed++;
    printf("%s  repetition  expected h7h8  got %s\n", ok ? "PASS" : "FAIL", str);

    // quiescence check: Qxd5 wins a pawn but loses the queen to cxd5.
    // a depth 1 search that stops right after the capture would play it
    parse_fen(&b, "4k3/8/2p5/3p4/8/8/3Q4/4K3 w - - 0 1");
    move_to_str(search(&b, 1, 1000), str);
    ok = strcmp(str, "d2d5") != 0;
    if (!ok) failed++;
    printf("%s  defended pawn  expected anything but d2d5  got %s\n", ok ? "PASS" : "FAIL", str);

    return failed;
}

int main(int argc, char **argv) {
    init_tables();

    // no arguments means a chess gui is talking to us
    if (argc == 1) {
        uci_loop();
        return 0;
    }

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

    fprintf(stderr, "usage: pufferfish                      (uci mode)\n"
                    "       pufferfish test\n"
                    "       pufferfish perft <depth> [\"fen\"]\n");
    return 1;
}
