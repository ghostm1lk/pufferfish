#ifndef PUFFERFISH_H
#define PUFFERFISH_H

#include <stdint.h>

typedef uint64_t U64;
typedef uint16_t Move;

enum { WHITE, BLACK, BOTH };
enum { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum { NO_SQ = 64 };
enum { CASTLE_WK = 1, CASTLE_WQ = 2, CASTLE_BK = 4, CASTLE_BQ = 8 };

#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

// move = from (6 bits) | to (6 bits) | flags (4 bits)
enum {
    QUIET = 0,
    DOUBLE_PUSH = 1,
    KING_CASTLE = 2,
    QUEEN_CASTLE = 3,
    CAPTURE = 4,        // any flag with this bit set is a capture
    EP_CAPTURE = 5,
    PROMO = 8,          // promoted piece is KNIGHT + (flags & 3)
    PROMO_CAPTURE = 12
};

#define MOVE(from, to, flags) ((Move)((from) | ((to) << 6) | ((flags) << 12)))
#define FROM(m) ((m) & 63)
#define TO(m) (((m) >> 6) & 63)
#define FLAGS(m) ((m) >> 12)

typedef struct {
    U64 pieces[2][6];   // [color][piece type]
    U64 occ[3];         // white, black, both
    int side;
    int ep;             // en passant target square, or NO_SQ
    int castle;
    int halfmove;
    int fullmove;
} Board;

typedef struct {
    Move moves[256];
    int count;
} MoveList;

static inline int pop_lsb(U64 *bb) {
    int sq = __builtin_ctzll(*bb);
    *bb &= *bb - 1;
    return sq;
}

// board.c
extern U64 knight_attacks[64], king_attacks[64], pawn_attacks[2][64];
void init_tables(void);
U64 bishop_attacks(int sq, U64 occ);
U64 rook_attacks(int sq, U64 occ);
int is_attacked(const Board *b, int sq, int by);
int parse_fen(Board *b, const char *fen);
int make_move(Board *b, Move m);
void move_to_str(Move m, char *out);

// movegen.c
void generate_moves(const Board *b, MoveList *list);

// search.c
Move search(const Board *b, int max_depth, int time_ms);

// uci.c
void uci_loop(void);

#endif
