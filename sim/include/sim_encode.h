// State encoder for the value network (ai/DESIGN.md section 1). One call turns a battle state, seen from one
// side, into fixed-size float and int arrays that Python views as tensors. Singles only (one active per side).
//
// Token order (SIMENC_TOKENS = 63):
//   0..5    my party slots 0..5          (mon tokens)
//   6..11   opponent party slots 0..5
//   12..59  move tokens: 12 + mon*4 + slot
//   60      my side, 61 opponent side, 62 field
// Float layout: mon[12][SIMENC_MON_F], move[48][SIMENC_MOVE_F], side[2][SIMENC_SIDE_F], field[SIMENC_FIELD_F].
// Int layout (SIMENC_INTS): monItem[12], monAbility[12], moveId[48], tokCat[63], present[63], terminal, pad.
#ifndef SIM_ENCODE_H
#define SIM_ENCODE_H

#include <stdint.h>

#define SIMENC_MONS    12
#define SIMENC_MOVES   48
#define SIMENC_TOKENS  63
#define SIMENC_MON_F   128
#define SIMENC_MOVE_F  64
#define SIMENC_SIDE_F  16
#define SIMENC_FIELD_F 16
#define SIMENC_FLOATS  (SIMENC_MONS * SIMENC_MON_F + SIMENC_MOVES * SIMENC_MOVE_F + 2 * SIMENC_SIDE_F + SIMENC_FIELD_F)
#define SIMENC_INTS    200

// int offsets
#define SIMENC_I_ITEM     0
#define SIMENC_I_ABILITY  12
#define SIMENC_I_MOVEID   24
#define SIMENC_I_TOKCAT   72
#define SIMENC_I_PRESENT  135
#define SIMENC_I_TERMINAL 198   // 0 not terminal, 1 `side` won, 2 lost, 3 draw

// token categories (tokCat)
enum
{
    SIMENC_CAT_MY_ACTIVE_MON, SIMENC_CAT_MY_BENCH_MON, SIMENC_CAT_OPP_ACTIVE_MON, SIMENC_CAT_OPP_BENCH_MON,
    SIMENC_CAT_MY_ACTIVE_MOVE, SIMENC_CAT_MY_BENCH_MOVE, SIMENC_CAT_OPP_ACTIVE_MOVE, SIMENC_CAT_OPP_BENCH_MOVE,
    SIMENC_CAT_MY_SIDE, SIMENC_CAT_OPP_SIDE, SIMENC_CAT_FIELD, SIMENC_CAT_COUNT
};

struct BattleSim;
// Encodes `sim` from the perspective of `side` (0 player, 1 opponent). Returns the terminal code.
int Sim_EncodeState(struct BattleSim *sim, int side, float *outF, int32_t *outI);

#endif
