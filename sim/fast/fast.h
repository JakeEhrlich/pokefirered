// Fast FireRed singles battle engine (from scratch; the verbatim port in ../src is the reference/oracle).
//
// Design: a compact, portable state (no pointers, fixed arrays, integer math only) and a one-turn step
// function. Randomness goes through three typed draws (fs_chance / fs_roll / fs_tie) so the same code can
// later enumerate outcomes with probabilities. It is NOT RNG-compatible with the verbatim engine: only the
// outcome distributions must agree (tests/fastdiff.c compares them per position and joint action).
// Coverage is explicit: every move effect, ability and item is implemented or reported by fs_unsupported();
// callers fall back to the verbatim engine for positions that carry anything unsupported.
#ifndef FAST_H
#define FAST_H

#include <stdint.h>
#include <string.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;

#define FS_PARTY 6
#define FS_MOVES 4
#define FS_STAGES 8   // hp(unused) atk def spe spa spd acc eva (game order)

enum { FS_SIDE_PLAYER = 0, FS_SIDE_OPP = 1 };
enum { FS_ACT_MOVE = 0, FS_ACT_SWITCH = 2, FS_ACT_NONE = 0xFF };   // same numbering as the verbatim engine's B_ACTION_*; NONE = cancelled
enum { FS_REQ_TURN = 1, FS_REQ_SWITCH = 2, FS_REQ_DONE = 3 };
enum { FS_OUTCOME_NONE = 0, FS_OUTCOME_P0_WON = 1, FS_OUTCOME_P1_WON = 2, FS_OUTCOME_DRAW = 3 };

// status1 bits (same as the game)
#define FS_S1_SLEEP    0x07
#define FS_S1_PSN      0x08
#define FS_S1_BRN      0x10
#define FS_S1_FRZ      0x20
#define FS_S1_PAR      0x40
#define FS_S1_TOX      0x80
#define FS_S1_TOXCTR   0xF00     // toxic counter << 8
#define FS_S1_ANY      0xFF

// volatile flags on an active battler (fs_battler.vol)
#define FS_V_CONFUSED    (1u << 0)
#define FS_V_FLINCH      (1u << 1)
#define FS_V_FOCUS_ENERGY (1u << 2)
#define FS_V_SUBSTITUTE  (1u << 3)
#define FS_V_LEECH_SEED  (1u << 4)
#define FS_V_RECHARGE    (1u << 5)
#define FS_V_PROTECTED   (1u << 6)   // this turn
#define FS_V_ENDURED     (1u << 7)   // this turn
#define FS_V_ROOTED      (1u << 8)
#define FS_V_CURSED      (1u << 9)
#define FS_V_NIGHTMARE   (1u << 10)
#define FS_V_TORMENT     (1u << 11)
#define FS_V_FORESIGHT   (1u << 12)
#define FS_V_DEFENSE_CURL (1u << 13)
#define FS_V_CHARGED     (1u << 14)
#define FS_V_MINIMIZED   (1u << 15)
#define FS_V_MUD_SPORT   (1u << 16)
#define FS_V_WATER_SPORT (1u << 17)
#define FS_V_ESCAPE_PREV (1u << 18)
#define FS_V_DESTINY_BOND (1u << 19)
#define FS_V_INFATUATED  (1u << 20)
#define FS_V_TRANSFORMED (1u << 21)
#define FS_V_ON_AIR      (1u << 22)
#define FS_V_UNDERGROUND (1u << 23)
#define FS_V_UNDERWATER  (1u << 24)
#define FS_V_IMPRISON    (1u << 25)
#define FS_V_GRUDGE      (1u << 26)
#define FS_V_MOVED_THIS_TURN (1u << 27)
#define FS_V_FLASH_FIRE  (1u << 28)
#define FS_V_TRUANT_LOAF (1u << 29)
#define FS_V_INTIMIDATE_PENDING (1u << 30)   // Intimidate not yet applied (no target when it entered; STATUS3_INTIMIDATE_POKES)
#define FS_V_TRACE_ARMED (1u << 31)   // Trace has not copied an ability yet (STATUS3_TRACE)

typedef struct
{
    u16 species;
    u16 hp, maxHP, atk, def, spe, spa, spd;
    u16 moves[FS_MOVES];
    u8 pp[FS_MOVES];
    u8 maxPP[FS_MOVES];
    u16 item;
    u8 ability, level, type1, type2, gender;
    u16 status1;          // FS_S1_* (sleep count in bits 0..2, toxic counter in bits 8..11)
    u8 hpType, hpPower;   // Hidden Power
    u8 friendship;
    u8 weightIdx;         // unused for now
    u8 nature;            // personality % 25 (the confusion berries check the nature's disliked flavor)
} fs_mon;

typedef struct
{
    // the battle copy of the active mon (like gBattleMons; Transform / type changes live here)
    u16 species, hp, maxHP, atk, def, spe, spa, spd;
    u16 moves[FS_MOVES];
    u8 pp[FS_MOVES];
    u16 item;
    u8 ability, level, type1, type2, gender;
    u16 status1;
    u8 monIdx;            // party slot
    u8 present;           // 0 when the slot is empty (fainted, awaiting replacement)
    s8 stages[FS_STAGES]; // stat stages, 0..12 with 6 = neutral (game encoding)
    u32 vol;              // FS_V_*
    u8 confusionTurns, uproarTurns, bideTurns, lockTurns, wrapTurns, sleepTalk;
    u8 disableTimer, encoreTimer, tauntTimer, perishTimer, rolloutTimer, chargeTimer, furyCutter, stockpile, substituteHP, protectUses, truantCounter, rechargeTimer, yawnTimer;
    u8 disabledPos, encoredPos, isFirstTurn;
    u16 lockedMove, lastMove, lastLandedMove, lastHitByType, chosenMove, bideDmg, wrapMove;
    u16 choicedMove;      // Choice Band lock (0 = none)
    u8 lastMoveTarget;
    u8 lastHitPhysical;   // Counter / Mirror Coat category of the last hit taken this turn: Hidden Power counts as its listed Normal type (physical) in datahpupdate
    u8 hpTypeCache;
    u8 hpPowerCache;      // Hidden Power power (from the battle copy's IVs: Transform copies them)
    u8 mimicked;          // bitmask of move slots replaced by Mimic (their PP is not written back to the party)
    u8 lockOn;            // Lock-On / Mind Reader turns left on this mon (the opponent's moves cannot miss it)
    // multi-turn moves (fast_ext.c): lockedMove != 0 is the game's STATUS2_MULTIPLETURNS + gLockedMoves (no choice
    // next turn); takenDmg is gTakenDmg (Bide); unable is WasUnableToUseMove() for this turn (Uproar / Thrash end);
    // rage is STATUS2_RAGE
    u16 takenDmg;
    u8 unable, rage;
} fs_battler;

typedef struct
{
    fs_mon party[FS_PARTY];
    fs_battler act;
    u8 reflect, lightscreen, mist, safeguard, spikes;
    u8 wishTurns, wishMon, futureSightTurns;
    u16 futureSightDmg, futureSightMove;
    u8 futureSightFromSide;
    u8 knockedOff;        // party slots whose item was knocked off (gWishFutureKnock.knockedOffMons): the party keeps the item, the battle copy loses it
    u16 usedItem;         // the last held item consumed in this battler slot (gBattleStruct->usedHeldItems: Recycle)
} fs_side;

typedef struct { u8 type; u8 slot; } fs_action;   // FS_ACT_MOVE: move slot 0..3 (4 = Struggle); FS_ACT_SWITCH: party slot

typedef struct
{
    fs_side side[2];
    u8 weather;           // 0 none, 1 rain, 2 sun, 3 sand, 4 hail
    u8 weatherTurns;      // remaining, 0xFF permanent
    u16 turn;
    u8 request;           // FS_REQ_*
    u8 switchMask;        // FS_REQ_SWITCH: sides that must replace (bit 0 player, bit 1 opp)
    u8 batonMask;         // FS_REQ_SWITCH: sides whose switch is a Baton Pass (the active mon is still in; stages etc. carry over)
    u8 phase;             // 0: at a turn start; 1: inside the action phase (replacements come right after a KO; later actions still run); 2: after the end-of-turn effects
    u8 orderN, orderPos;  // the turn's action order (sides) and how far it has run
    u8 order[2];
    fs_action pending[2]; // the chosen action of each side this turn
    u8 outcome;           // FS_OUTCOME_*
    u16 maxTurns;
    u32 rng;
    u32 unsupported;      // accumulated FS_UNSUP_* reasons hit during steps (0 = clean)
} fs_state;


// unsupported reasons (bitmask)
#define FS_UNSUP_MOVE_EFFECT   (1u << 0)
#define FS_UNSUP_ABILITY       (1u << 1)
#define FS_UNSUP_ITEM          (1u << 2)
#define FS_UNSUP_VOLATILE      (1u << 3)
#define FS_UNSUP_DOUBLES       (1u << 4)
#define FS_UNSUP_STATE         (1u << 5)   // a verbatim state that cannot be imported (mid-move, etc.)
#define FS_UNSUP_SPECIES       (1u << 6)

struct BattleSim;

// ---- setup / import
// Imports a verbatim engine state that is at a decision point (turn start: battler 0's action request, or a
// forced replacement request). Returns 0, or -1 (with out->unsupported set) when it cannot be represented.
int fs_import(fs_state *out, const struct BattleSim *sim);
// Bitmask of unsupported elements present anywhere in the position (both parties): 0 means fully supported.
u32 fs_unsupported(const fs_state *s);
void fs_seed(fs_state *s, u32 seed);

// ---- play
int fs_legal_actions(const fs_state *s, int side, fs_action *out);   // count; at FS_REQ_SWITCH: switches only
// Applies one decision. At FS_REQ_TURN both actions are used; at FS_REQ_SWITCH only the sides in switchMask
// (the other action is ignored). Runs to the next decision point. Returns the new request kind.
int fs_step(fs_state *s, fs_action a0, fs_action a1);

// ---- evaluation helpers
float fs_value_basic(const fs_state *s, int side);   // the same heuristic as Sim_ValueBasic, on the fast state

// ---- randomness (xorshift32 on s->rng)
static inline u32 fs_rand(fs_state *s) { u32 x = s->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x; return x; }
static inline int fs_chance(fs_state *s, u32 num, u32 den) { return (fs_rand(s) % den) < num; }   // true with probability num/den
static inline u32 fs_roll(fs_state *s, u32 n) { return fs_rand(s) % n; }                            // uniform in [0, n)

// ---- data (from the verbatim engine's tables; linked from libsim)
const char *fs_move_name(u16 move);

#endif
