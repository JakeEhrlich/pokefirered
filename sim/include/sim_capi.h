// Flat C API of the simulator for other languages (Python via ctypes: see py/frlgsim). A battle state is an
// opaque blob of sim_state_size() bytes that the caller allocates; every function takes a pointer to it.
// Actions are 6 bytes: type, moveSlot, target, partySlot, item low, item high (struct SimAction layout).
#ifndef SIM_CAPI_H
#define SIM_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t sim_state_size(void);
size_t sim_pokemon_size(void);                       // 100

// setup
void sim_init(void *sim, uint32_t battleTypeFlags, uint16_t seed);   // flags: 8 = trainer, +1 = double
int  sim_set_team_tsv(void *sim, int side, const char *rows);        // rows: the flat TSV of one team (tools/teams.py tsv); returns mons loaded
int  sim_set_party_bytes(void *sim, int side, const void *party600); // raw struct Pokemon[6]
void sim_get_party_bytes(const void *sim, int side, void *out600);
int  sim_load_trainer(void *sim, uint16_t trainerId);                // the game's trainer party (enemy side)
void sim_set_trainer_id(void *sim, uint16_t trainerId);              // for the game AI's flags (see sim_trainer_for_ai_flags)
uint16_t sim_trainer_for_ai_flags(uint32_t aiFlags);                 // 7 = the usual "smart" trainer AI
void sim_set_rng(void *sim, uint32_t seed);                          // engine xorshift RNG
void sim_set_policy(void *sim, int side, int policy);                // 0 none (ask the caller), 1 random, 2 the game's AI (enemy side only)
void sim_set_strict(void *sim, int strict);                          // 1: sim_answer rejects illegal actions
void sim_set_max_turns(void *sim, int maxTurns);
int  sim_start(void *sim);                                           // 0 ok, -1/-2/-3 (see Sim_Start)

// play
int  sim_run(void *sim);                                             // 0 request pending, 1 finished, 2 stuck, 3 error
int  sim_request_battler(const void *sim);
int  sim_request_kind(const void *sim);                              // 1 action, 2 switch, 0 none
int  sim_answer(void *sim, int battler, const uint8_t action[6]);    // 0 accepted, -1 rejected
int  sim_legal_actions(const void *sim, int battler, uint8_t *out, int maxActions); // returns count; out: 6 bytes each
int  sim_legal_switches(const void *sim, int battler, uint8_t *outSlots, int maxOut);
int  sim_finished(const void *sim);
int  sim_outcome(const void *sim);                                   // 1 player side won, 2 lost, 3 draw
int  sim_turn(const void *sim);
uint32_t sim_rng_value(const void *sim);
uint32_t sim_rng_calls(const void *sim);
int  sim_error(const void *sim);

// Game setup the way the arena does it: trainer singles, engine xorshift seeded, strict answers. Returns Sim_Start's result.
int  sim_setup_game(void *sim, const void *partyA600, const void *partyB600, uint32_t seed, int maxTurns);
int  sim_party_from_tsv(const char *rows, void *out600);          // builds up to 6 mons from TSV rows; returns the count

// ---- value-network encoding (include/sim_encode.h)
void sim_enc_sizes(int32_t out[16]);                              // floats, ints, mons, moves, tokens, monF, moveF, sideF, fieldF, catCount
int  sim_encode(void *sim, int side, float *outF, int32_t *outI); // returns the terminal code (0 none, 1 side won, 2 lost, 3 draw)
// Simulates every (mine[i], theirs[j]) pair from the turn-start state for `samples` engine seeds, each until the
// next decision of either side (any kind) or the end, and encodes the result from `side`'s perspective. Writes
// nMine*nTheirs*samples records (index (i*nTheirs + j)*samples + s) into outF/outI. Returns the count.
int  sim_joint_encode(const void *turnStart, int me, const uint8_t *mine, int nMine, const uint8_t *theirs, int nTheirs,
                      int samples, uint32_t seed, int side, float *outF, int32_t *outI);
// Same for a single answer from the current state (a mid-turn replacement; another battler's pending request is
// answered randomly first). Writes n*samples records.
int  sim_after_encode(const void *sim, int me, const uint8_t *acts, int n, int samples, uint32_t seed, int side, float *outF, int32_t *outI);
void sim_regret_matching(const float *M, int n, int m, int iters, int plus, int alternating, int linearAvg, float *sigmaRow, float *sigmaCol);

// ---- built-in agents (sim_agent.h specs)
void *sim_agent_new(const char *spec, uint32_t seed);              // NULL on a bad spec
void  sim_agent_free(void *agent);
const char *sim_agent_name(const void *agent);
// Decides for `battler`'s pending request; turnStart may be NULL (the arena passes the turn-start state for
// action requests). Writes a 6-byte action. Returns 0, or -1 when the agent cannot play this side (the game AI on the player side).
int  sim_agent_decide(void *agent, void *sim, const void *turnStart, int battler, int kind, uint8_t out[6]);

float sim_value_basic(const void *sim, int side);                    // the hand-tuned heuristic, for comparisons
float sim_value_material(const void *sim, int side);

// state accessors for feature building (ints; -1 where not applicable)
//   party mon (32 ints): species, level, hp, maxHP, status1, item, abilityNum, ability, moves[4], pp[4], maxpp[4], stats[6] (hp,atk,def,spe,spa,spd),
//                        types[2], happiness, isEgg, gender, personality&0xff
int  sim_get_party_mon(const void *sim, int side, int slot, int32_t out[32]);
//   battler (64 ints): species, hp, maxHP, level, item, ability, types[2], moves[4], pp[4], stats[6], stages[8], status1, status2, status3,
//                      disable: disabledMove, disableTimer, encoredMove, encoredMovePos, encoreTimer, protectUses, stockpile, substituteHP,
//                      perishTimer, rolloutTimer, chargeTimer, tauntTimer, furyCutter, isFirstTurn, truant, rechargeTimer, battlerWithSureHit,
//                      lastUsedMove, lastLandedMove, lastResultingMove, partyIndex, absent, wishCounter, futureSightCounter, futureSightDmg, futureSightAttacker
int  sim_get_battler(const void *sim, int battler, int32_t out[64]);
//   side (16 ints): reflect, lightscreen, mist, safeguard, followme, spikes, hpOnSwitchout, alive, fainted
int  sim_get_side(const void *sim, int side, int32_t out[16]);
//   field (16 ints): weatherBits, weatherDuration, turn, battlersCount, typeFlags, requestKind, requestBattler
int  sim_get_field(const void *sim, int32_t out[16]);
int  sim_battlers_count(const void *sim);

// names (static strings)
const char *sim_species_name(int id);
const char *sim_move_name(int id);
const char *sim_item_name(int id);
const char *sim_ability_name(int id);
//   move data (8 ints): effect, power, type, accuracy, pp, secondaryEffectChance, target, priority, flags
int  sim_get_move_data(int move, int32_t out[16]);
//   species base data (16 ints): baseHP, baseAtk, baseDef, baseSpe, baseSpA, baseSpD, type1, type2, ability1, ability2, genderRatio, weight, height
int  sim_get_species_data(int species, int32_t out[16]);
//   item data: holdEffect, holdEffectParam, pocket, battleUsage
int  sim_get_item_data(int item, int32_t out[8]);

#ifdef __cplusplus
}
#endif
#endif
