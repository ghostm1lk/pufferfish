#include <stdio.h>
#include <time.h>
#include "pufferfish.h"

#define INF 32000
#define MATE 30000

static clock_t deadline;
static int stopped;
static U64 nodes;

// score from the side to move's point of view, in centipawns.
// ponytail: material only, so it has no idea where pieces belong. add piece-square tables next
static int evaluate(const Board *b) {
    static const int value[6] = {100, 320, 330, 500, 900, 0};
    int score = 0;
    for (int p = PAWN; p <= QUEEN; p++) {
        score += value[p] * __builtin_popcountll(b->pieces[b->side][p]);
        score -= value[p] * __builtin_popcountll(b->pieces[b->side ^ 1][p]);
    }
    return score;
}

static int in_check(const Board *b) {
    return is_attacked(b, __builtin_ctzll(b->pieces[b->side][KING]), b->side ^ 1);
}

// negamax with alpha-beta pruning. alpha is the best score we can already
// force, beta is the most the opponent will let us have. once alpha >= beta
// the opponent would never enter this line, so the remaining moves are skipped.
// ponytail: no quiescence search, so it misjudges positions in the middle of a
// trade. no repetition detection either, that needs position hashing
static int negamax(const Board *b, int depth, int ply, int alpha, int beta) {
    // checking the clock is slow, so only do it every 2048 nodes
    if ((++nodes & 2047) == 0 && clock() > deadline) stopped = 1;
    if (stopped) return 0;

    if (depth == 0) return evaluate(b);

    MoveList list;
    generate_moves(b, &list);

    int legal = 0;
    for (int i = 0; i < list.count; i++) {
        Board next = *b;
        if (!make_move(&next, list.moves[i])) continue;
        legal++;

        int score = -negamax(&next, depth - 1, ply + 1, -beta, -alpha);
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

    // no legal moves: checkmate or stalemate.
    // adding ply makes a faster mate score better than a slower one
    if (legal == 0) return in_check(b) ? -MATE + ply : 0;
    if (b->halfmove >= 100) return 0;  // fifty-move rule

    return alpha;
}

// iterative deepening: search depth 1, then 2, then 3... until time runs out.
// the shallow searches are cheap and mean there is always a move ready to play.
// returns 0 if the position has no legal moves
Move search(const Board *b, int max_depth, int time_ms) {
    clock_t start = clock();
    deadline = start + (clock_t)((double)time_ms * CLOCKS_PER_SEC / 1000);
    stopped = 0;
    nodes = 0;

    // keep only the legal moves, so even an interrupted search has one to return
    MoveList all, list;
    generate_moves(b, &all);
    list.count = 0;
    for (int i = 0; i < all.count; i++) {
        Board next = *b;
        if (make_move(&next, all.moves[i])) list.moves[list.count++] = all.moves[i];
    }
    if (list.count == 0) return 0;

    Move best = list.moves[0];
    for (int depth = 1; depth <= max_depth; depth++) {
        int alpha = -INF;
        int best_index = 0;

        for (int i = 0; i < list.count; i++) {
            Board next = *b;
            make_move(&next, list.moves[i]);
            int score = -negamax(&next, depth - 1, 1, -INF, -alpha);
            if (stopped) break;
            if (score > alpha) {
                alpha = score;
                best_index = i;
            }
        }

        // a search that ran out of time is incomplete, keep the last finished one
        if (stopped) break;

        // try the best move first at the next depth, it makes alpha-beta cut far more
        best = list.moves[best_index];
        list.moves[best_index] = list.moves[0];
        list.moves[0] = best;

        char str[6];
        move_to_str(best, str);
        long ms = (long)((double)(clock() - start) * 1000 / CLOCKS_PER_SEC);
        printf("info depth %d score cp %d nodes %llu time %ld pv %s\n",
               depth, alpha, (unsigned long long)nodes, ms, str);
    }
    return best;
}
