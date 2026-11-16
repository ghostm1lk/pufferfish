#include "pufferfish.h"

static void add(MoveList *list, int from, int to, int flags) {
    list->moves[list->count++] = MOVE(from, to, flags);
}

// a pawn reaching the last rank becomes four moves, one per promotion piece
static void add_pawn_move(MoveList *list, int from, int to, int flags) {
    if (to >= 56 || to <= 7) {
        for (int i = 0; i < 4; i++) add(list, from, to, flags | PROMO | i);
    } else {
        add(list, from, to, flags);
    }
}

static void pawn_moves(const Board *b, MoveList *list) {
    int us = b->side, them = us ^ 1;
    int up = us == WHITE ? 8 : -8;
    int start_rank = us == WHITE ? 1 : 6;
    U64 pawns = b->pieces[us][PAWN];

    while (pawns) {
        int from = pop_lsb(&pawns);

        // pushes. a pawn is never on the last rank, so from + up stays on the board
        int to = from + up;
        if (!(b->occ[BOTH] & (1ULL << to))) {
            add_pawn_move(list, from, to, QUIET);
            if ((from >> 3) == start_rank && !(b->occ[BOTH] & (1ULL << (to + up)))) {
                add(list, from, to + up, DOUBLE_PUSH);
            }
        }

        U64 targets = pawn_attacks[us][from] & b->occ[them];
        while (targets) add_pawn_move(list, from, pop_lsb(&targets), CAPTURE);

        if (b->ep != NO_SQ && (pawn_attacks[us][from] & (1ULL << b->ep))) {
            add(list, from, b->ep, EP_CAPTURE);
        }
    }
}

static void piece_moves(const Board *b, MoveList *list) {
    int us = b->side, them = us ^ 1;

    for (int piece = KNIGHT; piece <= KING; piece++) {
        U64 pieces = b->pieces[us][piece];
        while (pieces) {
            int from = pop_lsb(&pieces);
            U64 targets;
            switch (piece) {
                case KNIGHT: targets = knight_attacks[from]; break;
                case BISHOP: targets = bishop_attacks(from, b->occ[BOTH]); break;
                case ROOK:   targets = rook_attacks(from, b->occ[BOTH]); break;
                case QUEEN:  targets = bishop_attacks(from, b->occ[BOTH]) | rook_attacks(from, b->occ[BOTH]); break;
                default:     targets = king_attacks[from]; break;
            }
            targets &= ~b->occ[us];
            while (targets) {
                int to = pop_lsb(&targets);
                add(list, from, to, (b->occ[them] & (1ULL << to)) ? CAPTURE : QUIET);
            }
        }
    }
}

// castling needs: the right, empty squares between king and rook,
// and the king not in check or passing through an attacked square.
// the landing square isn't checked here, make_move rejects it like any other illegal move
static void castle_moves(const Board *b, MoveList *list) {
    int us = b->side, them = us ^ 1;
    int e = us == WHITE ? 4 : 60;  // king's home square, e1 or e8
    int kingside = us == WHITE ? CASTLE_WK : CASTLE_BK;
    int queenside = us == WHITE ? CASTLE_WQ : CASTLE_BQ;
    U64 occ = b->occ[BOTH];

    if ((b->castle & kingside) && !(occ & (3ULL << (e + 1)))
        && !is_attacked(b, e, them) && !is_attacked(b, e + 1, them)) {
        add(list, e, e + 2, KING_CASTLE);
    }
    if ((b->castle & queenside) && !(occ & (7ULL << (e - 3)))
        && !is_attacked(b, e, them) && !is_attacked(b, e - 1, them)) {
        add(list, e, e - 2, QUEEN_CASTLE);
    }
}

// generates pseudo-legal moves: they follow piece movement rules but may
// leave our own king in check. make_move filters those out
void generate_moves(const Board *b, MoveList *list) {
    list->count = 0;
    pawn_moves(b, list);
    piece_moves(b, list);
    castle_moves(b, list);
}
