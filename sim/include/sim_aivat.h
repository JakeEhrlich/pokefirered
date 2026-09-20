// AIVAT-style low-variance game scoring (Burch et al. 2018, "AIVAT: A New Variance Reduction Technique for
// Agent Evaluation in Imperfect Information Games"), adapted to this simultaneous-move game with chance.
//
// The estimate of side 0's expected score is the terminal result minus, at every decision point, control
// variates with zero conditional mean:
//   chance:   V(next decision state) - mean over K fresh samples of V(next state | s, a*, b*)
//   opponent: M(a*, b*) - sum_b sigma_b(b) M(a*, b)          (needs the acting agent's sampling distribution)
//   self:     sum_b sigma_b(b) M(a*, b) - sum_a sum_b sigma_a sigma_b M(a, b)
// where M(a, b) is the K-sample mean of V after joint action (a, b) and V is the hand heuristic
// (Sim_ValueBasic / fs_value_basic; +-1 at the end). Terms whose distribution is unknown are simply dropped,
// which keeps the estimate unbiased. The samples come from the fast engine when the position imports (same
// value function on both sides of every term), else from the verbatim engine.
#ifndef SIM_AIVAT_H
#define SIM_AIVAT_H

#include "sim.h"
#include "sim_agent.h"

struct SimAivatNode
{
    struct BattleSim *state;          // the decision-point state (before any answer)
    u8 kind, requester;               // SIM_REQ_ACTION (both sides act) or SIM_REQ_SWITCH (requester acts)
    u8 haveAct[2], havePolicy[2];
    struct SimAction act[2];
    float policy[2][SIM_AGENT_MAX_ACTIONS];
    int policyN[2];
};

struct SimAivat
{
    int K;                            // samples per joint cell
    u32 rng;
    double correction;                // running sum of the control variates (side 0's perspective)
    int nodes, nodesFast, nodesSkipped;
    long sims;
    u8 verbatimOnly;                  // 1: never use the fast engine
    // calibration of the baseline: the raw heuristic V is mapped through g(V) = tanh(calibK * atanh(V)) when
    // calibK > 0 (calibK = 1 is the identity; fitted from (V, outcome) pairs, see tools/aivat_calib.py)
    float calibK;
    u8 vf;                            // baseline: 0 = calibrated heuristic, 1 = one-turn lookahead of it (RM+ 30 over a K1-sample matrix, fast engine only)
    int K1;
    int nV; float vTrace[512];        // V at each decision point (for calibration dumps)
};

void Sim_AivatBegin(struct SimAivat *av, u32 seed, int K);
// Adds one decision point: `nd` describes the state and what was chosen there, `next` is the state at the
// following decision point (or the finished state).
void Sim_AivatNode(struct SimAivat *av, const struct SimAivatNode *nd, const struct BattleSim *next);
// Side 0's AIVAT score given the raw terminal score z (+1 win, -1 loss, 0 draw).
double Sim_AivatScore(const struct SimAivat *av, double z);

#endif
