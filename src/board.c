#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "pufferfish.h"

// squares are numbered a1 = 0, b1 = 1, ... h8 = 63

U64 knight_attacks[64], king_attacks[64], pawn_attacks[2][64];

// castling rights that survive a move touching each square
static int castle_mask[64];

// zobrist hashing: every (color, piece, square) gets a random 64-bit number,
// and a position's key is the xor of the numbers for everything on the board.
// xor undoes itself, so moving a piece is two cheap xors on the old key
static U64 zobrist_piece[2][6][64], zobrist_castle[16], zobrist_ep[8], zobrist_side;

// xorshift, a tiny random number generator. the seed is fixed so keys
// are the same on every run, which keeps bugs reproducible
static U64 random_u64(void) {
    static U64 state = 0x9E3779B97F4A7C15ULL;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// builds an attack mask from a list of (file, rank) steps
static U64 step_mask(int sq, const int steps[][2], int n) {
    U64 bb = 0;
    for (int i = 0; i < n; i++) {
        int f = (sq & 7) + steps[i][0];
        int r = (sq >> 3) + steps[i][1];
        if (f >= 0 && f < 8 && r >= 0 && r < 8) bb |= 1ULL << (r * 8 + f);
    }
    return bb;
}

void init_tables(void) {
    static const int knight[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
    static const int king[8][2] = {{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}};
    static const int white_pawn[2][2] = {{-1,1},{1,1}};
    static const int black_pawn[2][2] = {{-1,-1},{1,-1}};

    for (int sq = 0; sq < 64; sq++) {
        knight_attacks[sq] = step_mask(sq, knight, 8);
        king_attacks[sq] = step_mask(sq, king, 8);
        pawn_attacks[WHITE][sq] = step_mask(sq, white_pawn, 2);
        pawn_attacks[BLACK][sq] = step_mask(sq, black_pawn, 2);
        castle_mask[sq] = 15;
    }
    castle_mask[0] = ~CASTLE_WQ;                 // a1
    castle_mask[4] = ~(CASTLE_WK | CASTLE_WQ);   // e1
    castle_mask[7] = ~CASTLE_WK;                 // h1
    castle_mask[56] = ~CASTLE_BQ;                // a8
    castle_mask[60] = ~(CASTLE_BK | CASTLE_BQ);  // e8
    castle_mask[63] = ~CASTLE_BK;                // h8

    for (int c = WHITE; c <= BLACK; c++)
        for (int p = PAWN; p <= KING; p++)
            for (int sq = 0; sq < 64; sq++) zobrist_piece[c][p][sq] = random_u64();
    for (int i = 0; i < 16; i++) zobrist_castle[i] = random_u64();
    for (int i = 0; i < 8; i++) zobrist_ep[i] = random_u64();
    zobrist_side = random_u64();
}

// walks one direction until it hits a piece or the edge.
// the blocker's square is included, so captures fall out for free
static U64 ray(int sq, int df, int dr, U64 occ) {
    U64 bb = 0;
    int f = (sq & 7) + df;
    int r = (sq >> 3) + dr;
    while (f >= 0 && f < 8 && r >= 0 && r < 8) {
        U64 s = 1ULL << (r * 8 + f);
        bb |= s;
        if (occ & s) break;
        f += df;
        r += dr;
    }
    return bb;
}

// ponytail: ray loops are slow, swap for magic bitboards once search speed matters
U64 bishop_attacks(int sq, U64 occ) {
    return ray(sq, 1, 1, occ) | ray(sq, 1, -1, occ) | ray(sq, -1, 1, occ) | ray(sq, -1, -1, occ);
}

U64 rook_attacks(int sq, U64 occ) {
    return ray(sq, 1, 0, occ) | ray(sq, -1, 0, occ) | ray(sq, 0, 1, occ) | ray(sq, 0, -1, occ);
}

// is sq attacked by any piece of color `by`?
// trick: put each piece type on sq and see if it hits one of the enemy's
int is_attacked(const Board *b, int sq, int by) {
    const U64 *p = b->pieces[by];
    if (pawn_attacks[by ^ 1][sq] & p[PAWN]) return 1;
    if (knight_attacks[sq] & p[KNIGHT]) return 1;
    if (king_attacks[sq] & p[KING]) return 1;
    if (bishop_attacks(sq, b->occ[BOTH]) & (p[BISHOP] | p[QUEEN])) return 1;
    if (rook_attacks(sq, b->occ[BOTH]) & (p[ROOK] | p[QUEEN])) return 1;
    return 0;
}

static void update_occ(Board *b) {
    b->occ[WHITE] = b->occ[BLACK] = 0;
    for (int p = PAWN; p <= KING; p++) {
        b->occ[WHITE] |= b->pieces[WHITE][p];
        b->occ[BLACK] |= b->pieces[BLACK][p];
    }
    b->occ[BOTH] = b->occ[WHITE] | b->occ[BLACK];
}

// builds the key from scratch. make_move updates it incrementally instead,
// and the test suite checks that both always agree
U64 compute_key(const Board *b) {
    U64 key = 0;
    for (int c = WHITE; c <= BLACK; c++) {
        for (int p = PAWN; p <= KING; p++) {
            U64 bb = b->pieces[c][p];
            while (bb) key ^= zobrist_piece[c][p][pop_lsb(&bb)];
        }
    }
    key ^= zobrist_castle[b->castle];
    if (b->ep != NO_SQ) key ^= zobrist_ep[b->ep & 7];
    if (b->side == BLACK) key ^= zobrist_side;
    return key;
}

// returns 1 on success, 0 if the fen is malformed
int parse_fen(Board *b, const char *fen) {
    static const char letters[] = "pnbrqk";
    memset(b, 0, sizeof *b);

    // piece placement, rank 8 first
    int sq = 56;
    for (; *fen && *fen != ' '; fen++) {
        if (*fen == '/') {
            sq -= 16;
        } else if (*fen >= '1' && *fen <= '8') {
            sq += *fen - '0';
        } else {
            const char *p = strchr(letters, tolower((unsigned char)*fen));
            if (!p || sq < 0 || sq > 63) return 0;
            int color = islower((unsigned char)*fen) ? BLACK : WHITE;
            b->pieces[color][p - letters] |= 1ULL << sq;
            sq++;
        }
    }

    char side, castle[8], ep[8];
    b->fullmove = 1;
    if (sscanf(fen, " %c %7s %7s %d %d", &side, castle, ep, &b->halfmove, &b->fullmove) < 3) return 0;
    if (side != 'w' && side != 'b') return 0;
    b->side = side == 'w' ? WHITE : BLACK;

    if (strchr(castle, 'K')) b->castle |= CASTLE_WK;
    if (strchr(castle, 'Q')) b->castle |= CASTLE_WQ;
    if (strchr(castle, 'k')) b->castle |= CASTLE_BK;
    if (strchr(castle, 'q')) b->castle |= CASTLE_BQ;

    b->ep = NO_SQ;
    if (ep[0] != '-') {
        if (ep[0] < 'a' || ep[0] > 'h' || (ep[1] != '3' && ep[1] != '6')) return 0;
        b->ep = (ep[0] - 'a') + (ep[1] - '1') * 8;
    }

    // the rest of the engine assumes exactly one king per side
    if (__builtin_popcountll(b->pieces[WHITE][KING]) != 1) return 0;
    if (__builtin_popcountll(b->pieces[BLACK][KING]) != 1) return 0;

    update_occ(b);
    b->key = compute_key(b);
    return 1;
}

// adds or removes a piece, keeping the key in sync
static void toggle(Board *b, int color, int piece, int sq) {
    b->pieces[color][piece] ^= 1ULL << sq;
    b->key ^= zobrist_piece[color][piece][sq];
}

// plays a pseudo-legal move on the board.
// returns 0 if it leaves our own king in check (the caller throws the board away).
// there is no unmake: callers copy the board first, which is simpler and about as fast
int make_move(Board *b, Move m) {
    int from = FROM(m), to = TO(m), flags = FLAGS(m);
    int us = b->side, them = us ^ 1;
    int piece = piece_on(b, us, from);

    // take the old en passant and castling state out of the key.
    // the new state goes back in further down
    if (b->ep != NO_SQ) b->key ^= zobrist_ep[b->ep & 7];
    b->key ^= zobrist_castle[b->castle];

    if (flags & CAPTURE) {
        // en passant is the one capture where the victim isn't on the target square
        int victim_sq = to;
        if (flags == EP_CAPTURE) victim_sq = us == WHITE ? to - 8 : to + 8;
        toggle(b, them, piece_on(b, them, victim_sq), victim_sq);
    }

    toggle(b, us, piece, from);
    toggle(b, us, (flags & PROMO) ? KNIGHT + (flags & 3) : piece, to);

    // castling also moves the rook
    if (flags == KING_CASTLE) {
        toggle(b, us, ROOK, to + 1);
        toggle(b, us, ROOK, to - 1);
    }
    if (flags == QUEEN_CASTLE) {
        toggle(b, us, ROOK, to - 2);
        toggle(b, us, ROOK, to + 1);
    }

    b->ep = flags == DOUBLE_PUSH ? (from + to) / 2 : NO_SQ;
    b->castle &= castle_mask[from] & castle_mask[to];
    if (b->ep != NO_SQ) b->key ^= zobrist_ep[b->ep & 7];
    b->key ^= zobrist_castle[b->castle];
    b->key ^= zobrist_side;

    b->halfmove = (piece == PAWN || (flags & CAPTURE)) ? 0 : b->halfmove + 1;
    if (us == BLACK) b->fullmove++;
    b->side = them;
    update_occ(b);

    return !is_attacked(b, __builtin_ctzll(b->pieces[us][KING]), them);
}

// writes uci notation like "e2e4" or "e7e8q". out needs room for 6 chars
void move_to_str(Move m, char *out) {
    int from = FROM(m), to = TO(m), flags = FLAGS(m);
    out[0] = 'a' + (from & 7);
    out[1] = '1' + (from >> 3);
    out[2] = 'a' + (to & 7);
    out[3] = '1' + (to >> 3);
    out[4] = (flags & PROMO) ? "nbrq"[flags & 3] : '\0';
    out[5] = '\0';
}
