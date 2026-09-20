// Agents: things that answer the simulator's decision requests. Built-in ones range from random to
// regret-matching over a one-turn payoff matrix computed by simulating every joint action pair.
#ifndef SIM_AGENT_H
#define SIM_AGENT_H

#include "sim.h"

struct SimAgent;

// Zero-sum value of the current state for `side` (0 = player side), in [-1, 1]; terminal states are +-1/0.
typedef float (*SimValueFn)(struct BattleSim *sim, u8 side, void *ctx);

// `sim` has a pending request for `battler` of kind `kind`. `turnStart` is the state at the first decision
// of this turn, before either side committed (NULL when not available, e.g. mid-turn replacements); value
// agents simulate joint actions from it so the second mover cannot see the first mover's choice.
typedef void (*SimAgentDecideFn)(struct SimAgent *ag, struct BattleSim *sim, const struct BattleSim *turnStart,
                                 u8 battler, u8 kind, struct SimAction *out);

struct SimAgent
{
    char name[96];
    char spec[160];
    SimAgentDecideFn decide;   // NULL for the game's own AI (played through the engine's policy hook)
    u8 isGameAI;
    u32 aiFlags;               // game AI: AI_SCRIPT_* flags
    SimValueFn value;
    void *valueCtx;
    int iterations;            // regret matching iterations
    int samples;               // RNG samples per simulated action pair
    float epsilon;             // probability of a uniformly random legal action
    float floor;               // minimum sampling probability per action (after the strategy is computed)
    u8 plus;                   // RM+ (regrets clipped at 0)
    u8 alternating;            // alternate updates between the two players
    u8 linearAvg;              // weight iteration t by t when averaging strategies
    u32 rng;                   // the agent's own xorshift state (not the engine's RNG)
    // statistics
    u32 decisions, matrixCells, simulatedTurns;
};

// Builds an agent from "name[:key=value,...]". Names: game (flags=basic|smart|all), random, movebias
// (moves=0.85), greedy, epsgreedy (eps=0.1), expect, epsexpect (eps=0.1), rm (iters=10), rmplus
// (iters=10, alt=1, linavg=1), rmsample (iters=100: RM+ with fresh engine samples every iteration instead of
// a precomputed matrix). Common keys: vf=basic|material, samples=N, floor=P, eps=P, seed=N.
// Returns 0 on success, -1 on a bad spec (with a message in ag->name).
int Sim_AgentFromSpec(struct SimAgent *ag, const char *spec);
u32 Sim_AgentRandom(struct SimAgent *ag);

// Value functions.
float Sim_ValueBasic(struct BattleSim *sim, u8 side, void *ctx);      // HP, status, stages, screens, hazards
float Sim_ValueMaterial(struct BattleSim *sim, u8 side, void *ctx);   // HP fractions only

// Helpers usable by other agents.
int Sim_EstimateDamage(struct BattleSim *sim, u8 attacker, u8 defender, u8 moveSlot); // expected damage, no crit
int Sim_EstimateDamageMons(struct BattlePokemon *atk, struct BattlePokemon *def, u16 move, u8 atkBattler, u8 defBattler, int *rawOut);
int Sim_TypeMultiplier(u8 moveType, u8 defType1, u8 defType2);                       // x100
int Sim_TypeMatchupScore(struct BattleSim *sim, u16 species, u8 oppBattler);        // offensive - defensive, x10
u16 Sim_TrainerForAIFlags(u32 aiFlags);  // a trainer id with exactly these AI flags and no items (game AI)
// Simulates `mine` for `me` and `theirs` for the opponent from the turn-start state, resolving mid-turn
// replacements randomly, until the next decision or the end. Returns the value for `me`'s side.
float Sim_SimulateJoint(struct SimAgent *ag, const struct BattleSim *turnStart, u8 me, const struct SimAction *mine,
                        const struct SimAction *theirs, u32 seed);
// Simulates a single answer from the current state (the other side plays randomly meanwhile).
float Sim_SimulateAfter(struct SimAgent *ag, const struct BattleSim *sim, u8 me, const struct SimAction *act, u32 seed);
// Regret matching on an n x m zero-sum matrix (row player's payoff). Writes the average strategies.
void Sim_RegretMatching(const float *M, int n, int m, int iters, int plus, int alternating, int linearAvg,
                        float *sigmaRow, float *sigmaCol);

#endif
