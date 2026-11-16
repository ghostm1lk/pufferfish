#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pufferfish.h"

#define INF 32000
#define MATE 30000

static clock_t deadline;
static int stopped;
static U64 nodes;

U64 history[1024];
int history_len;

// transposition table: a cache of positions already searched, indexed by
// zobrist key. the same position is reached through many move orders, so
// this saves repeating whole subtrees. it also remembers each position's
// best move, which is the best move-ordering hint there is.
// ponytail: fixed 16 MB and always overwrites. add a uci Hash option and a
// smarter replacement rule if long games start thrashing it
enum { EXACT, LOWER, UPPER };

typedef struct {
    U64 key;
    Move move;
    int16_t score;
    int8_t depth;
    uint8_t flag;   // alpha-beta often only proves a bound, not an exact score
} Entry;

#define TABLE_SIZE (1 << 20)
static Entry table[TABLE_SIZE];

void clear_hash(void) {
    memset(table, 0, sizeof table);
}

// mate scores count plies from the root, but a table entry can be reused at
// a different distance from the root. so store them as "mate in n from here"
static int to_table(int score, int ply) {
    if (score > MATE - 1000) return score + ply;
    if (score < -MATE + 1000) return score - ply;
    return score;
}

static int from_table(int score, int ply) {
    if (score > MATE - 1000) return score - ply;
    if (score < -MATE + 1000) return score + ply;
    return score;
}

// has this position occurred before, in the game or earlier in this line?
// only positions since the last capture or pawn move can match, and only
// every second one has the same side to move.
// history must end with the position b was reached from, so the entry
// two from the end is the most recent one with b's side to move
static int is_repetition(const Board *b) {
    for (int i = history_len - 2; i >= 0 && i >= history_len - b->halfmove; i -= 2) {
        if (history[i] == b->key) return 1;
    }
    return 0;
}

// counts the node and checks the clock. reading the clock is slow,
// so it only happens every 2048 nodes
static int out_of_time(void) {
    if ((++nodes & 2047) == 0 && clock() > deadline) stopped = 1;
    return stopped;
}

static int in_check(const Board *b) {
    return is_attacked(b, __builtin_ctzll(b->pieces[b->side][KING]), b->side ^ 1);
}

// move ordering. alpha-beta prunes far more when good moves are tried first,
// so guess which moves are good: the table's move for this position first,
// then captures before quiet moves, and among captures the most valuable
// victim taken by the least valuable attacker (pawn takes queen before
// queen takes pawn). this is called mvv-lva
static int score_move(const Board *b, Move m, Move table_move) {
    if (m == table_move) return 10000;
    int flags = FLAGS(m);
    int score = 0;
    if (flags & CAPTURE) {
        int victim = flags == EP_CAPTURE ? PAWN : piece_on(b, b->side ^ 1, TO(m));
        score = 1000 + 10 * victim - piece_on(b, b->side, FROM(m));
    }
    if ((flags & PROMO) && (flags & 3) == 3) score += 2000;  // queen promotion
    return score;
}

static void score_moves(const Board *b, const MoveList *list, int *scores, Move table_move) {
    for (int i = 0; i < list->count; i++) scores[i] = score_move(b, list->moves[i], table_move);
}

// swaps the best remaining move into slot i. cheaper than sorting the whole
// list, because a cutoff usually comes after the first move or two
static void pick_move(MoveList *list, int *scores, int i) {
    int best = i;
    for (int j = i + 1; j < list->count; j++) {
        if (scores[j] > scores[best]) best = j;
    }
    Move m = list->moves[i];
    list->moves[i] = list->moves[best];
    list->moves[best] = m;
    int s = scores[i];
    scores[i] = scores[best];
    scores[best] = s;
}

// quiescence search: at the end of the main search, keep playing captures
// until the position is quiet, and only then trust the evaluation. without
// this the engine stops in the middle of a trade and thinks it won a piece.
// ponytail: generates every move and skips the quiet ones. a captures-only
// generator would be faster
static int quiesce(const Board *b, int alpha, int beta) {
    if (out_of_time()) return 0;

    // "stand pat": we are never forced to capture, so the score of doing
    // nothing is a floor on what this position is worth
    int stand_pat = evaluate(b);
    if (stand_pat >= beta) return beta;
    if (stand_pat > alpha) alpha = stand_pat;

    MoveList list;
    int scores[256];
    generate_moves(b, &list);
    score_moves(b, &list, scores, 0);

    for (int i = 0; i < list.count; i++) {
        pick_move(&list, scores, i);
        if (scores[i] == 0) break;  // only quiet moves are left

        Board next = *b;
        if (!make_move(&next, list.moves[i])) continue;

        int score = -quiesce(&next, -beta, -alpha);
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }
    return alpha;
}

// negamax with alpha-beta pruning. alpha is the best score we can already
// force, beta is the most the opponent will let us have. once alpha >= beta
// the opponent would never enter this line, so the remaining moves are skipped
static int negamax(const Board *b, int depth, int ply, int alpha, int beta) {
    if (depth == 0) return quiesce(b, alpha, beta);
    if (out_of_time()) return 0;

    // if this position was already searched at least this deep, reuse the result
    Entry *entry = &table[b->key & (TABLE_SIZE - 1)];
    Move table_move = 0;
    if (entry->key == b->key) {
        table_move = entry->move;
        if (entry->depth >= depth) {
            int score = from_table(entry->score, ply);
            if (entry->flag == EXACT) return score;
            if (entry->flag == LOWER && score >= beta) return score;
            if (entry->flag == UPPER && score <= alpha) return score;
        }
    }

    MoveList list;
    int scores[256];
    generate_moves(b, &list);
    score_moves(b, &list, scores, table_move);

    int old_alpha = alpha;
    Move best_move = 0;
    int legal = 0;
    for (int i = 0; i < list.count; i++) {
        pick_move(&list, scores, i);

        Board next = *b;
        if (!make_move(&next, list.moves[i])) continue;
        legal++;

        // a repeated position scores as a draw. one repetition is enough:
        // if it can happen once, it can happen again
        history[history_len++] = b->key;
        int score = is_repetition(&next) ? 0 : -negamax(&next, depth - 1, ply + 1, -beta, -alpha);
        history_len--;

        if (score > alpha) {
            alpha = score;
            best_move = list.moves[i];
        }
        if (alpha >= beta) break;
    }

    // no legal moves: checkmate or stalemate.
    // adding ply makes a faster mate score better than a slower one
    if (legal == 0) return in_check(b) ? -MATE + ply : 0;
    if (b->halfmove >= 100) return 0;  // fifty-move rule

    if (!stopped) {
        entry->key = b->key;
        entry->move = best_move;
        entry->score = (int16_t)to_table(alpha, ply);
        entry->depth = (int8_t)depth;
        // no move beat alpha: the real score is at most this.
        // cut off at beta: the real score is at least this
        entry->flag = alpha <= old_alpha ? UPPER : alpha >= beta ? LOWER : EXACT;
    }
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

    // keep only the legal moves, best guesses first.
    // this also means an interrupted search always has a move to return
    MoveList all, list;
    int scores[256];
    generate_moves(b, &all);
    score_moves(b, &all, scores, 0);
    list.count = 0;
    for (int i = 0; i < all.count; i++) {
        pick_move(&all, scores, i);
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
            history[history_len++] = b->key;
            int score = is_repetition(&next) ? 0 : -negamax(&next, depth - 1, 1, -INF, -alpha);
            history_len--;
            if (stopped) break;
            if (score > alpha) {
                alpha = score;
                best_index = i;
            }
        }

        // a search that ran out of time is incomplete, keep the last finished one
        if (stopped) break;

        // move the best move to the front for the next depth, keeping the rest in order
        best = list.moves[best_index];
        for (int i = best_index; i > 0; i--) list.moves[i] = list.moves[i - 1];
        list.moves[0] = best;

        char str[6];
        move_to_str(best, str);
        long ms = (long)((double)(clock() - start) * 1000 / CLOCKS_PER_SEC);
        printf("info depth %d score cp %d nodes %llu time %ld pv %s\n",
               depth, alpha, (unsigned long long)nodes, ms, str);
    }
    return best;
}
