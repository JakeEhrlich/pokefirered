// Copying, calling and random moves, and item/ability manipulation:
// Metronome, Sleep Talk, Assist, Mirror Move, Mimic, Sketch, Transform, Conversion, Conversion 2,
// Role Play, Skill Swap, Trick, Recycle, Knock Off, Thief/Covet, Nature Power, Secret Power, Camouflage.
#include "scenario.h"

// --- helpers ---

// The move a "calling" move (Metronome/Sleep Talk/Assist/Mirror Move/Nature Power) resolved to: the move of
// the first log entry of `battler` in `turn` after the caller's USEDMOVE entry whose move differs from the
// caller (a called two-turn move logs its charge turn with EMPTYSTRING3 rather than USEDMOVE). 0 if absent.
static u16 CalledMove(struct BattleSim *sim, u8 battler, int turn, u16 caller)
{
    int i, seen = 0;
    for (i = 0; i < sim->logCount; i++)
    {
        if (sim->log[i].battler != battler || sim->log[i].turn != turn)
            continue;
        if (!seen)
        {
            if (sim->log[i].stringId == STRINGID_USEDMOVE && sim->log[i].move == caller)
                seen = 1;
            continue;
        }
        if (sim->log[i].move != caller && sim->log[i].move != MOVE_NONE)
            return sim->log[i].move;
    }
    return 0;
}

static const u16 sMetronomeForbidden[] =
{
    MOVE_METRONOME, MOVE_STRUGGLE, MOVE_SKETCH, MOVE_MIMIC, MOVE_COUNTER, MOVE_MIRROR_COAT, MOVE_PROTECT,
    MOVE_DETECT, MOVE_ENDURE, MOVE_DESTINY_BOND, MOVE_SLEEP_TALK, MOVE_THIEF, MOVE_FOLLOW_ME, MOVE_SNATCH,
    MOVE_HELPING_HAND, MOVE_COVET, MOVE_TRICK, MOVE_FOCUS_PUNCH, MOVE_NONE
};
static int IsMetronomeForbidden(u16 move)
{
    int i;
    for (i = 0; i < (int)(sizeof(sMetronomeForbidden) / sizeof(sMetronomeForbidden[0])); i++)
        if (sMetronomeForbidden[i] == move)
            return 1;
    return 0;
}

static int LogHasCharge(struct BattleSim *sim)
{
    return LOG_HAS(STRINGID_PKMNFLEWHIGH) || LOG_HAS(STRINGID_PKMNDUGHOLE) || LOG_HAS(STRINGID_PKMNTOOKSUNLIGHT)
        || LOG_HAS(STRINGID_PKMNLOWEREDHEAD) || LOG_HAS(STRINGID_PKMNWHIPPEDWHIRLWIND) || LOG_HAS(STRINGID_PKMNISGLOWING)
        || LOG_HAS(STRINGID_PKMNHIDUNDERWATER);
}

// ===================== Metronome =====================

static void CheckMetronomeBasic(struct BattleSim *sim)
{
    u16 called = CalledMove(sim, 0, 0, MOVE_METRONOME);
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_METRONOME, 0), "metronome attack string");
    CHECK(called != MOVE_NONE, "a move was called");
    CHECK(!IsMetronomeForbidden(called), "called move %d is not in the forbidden list", called);
    CHECK(B(0).moves[0] == MOVE_METRONOME, "moveset unchanged");
    // Metronome's own script has no ppreduce; the called move's ppreduce hits the Metronome slot once.
    CHECK(PP(0, 0) == 9, "metronome pp deducted once (pp %d)", PP(0, 0));
}

static int WantMetronomeTwoTurn(struct BattleSim *sim) { return LogHasCharge(sim) && LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_METRONOME, 0); }
static void CheckMetronomeTwoTurn(struct BattleSim *sim)
{
    u16 called = CalledMove(sim, 0, 0, MOVE_METRONOME);
    CHECK(called != MOVE_NONE, "called a two-turn move (%d)", called);
    // The user is locked into the called move and finishes it on turn 2 without selecting Metronome again.
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, called, 1), "same move continued on turn 2");
    CHECK(!LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_METRONOME, 1), "metronome not used again on turn 2");
    CHECK(PP(0, 0) == 9, "only one pp used for the two-turn move (pp %d)", PP(0, 0));
    CHECK(!(STATUS2(0) & STATUS2_MULTIPLETURNS), "no longer locked after the second turn");
}

static int WantMimicVsMetronome(struct BattleSim *sim) { return LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_MIMIC, 1) && LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_METRONOME, 1) && OUTCOME() == 0; }
static void CheckMimicVsMetronome(struct BattleSim *sim)
{
    // The target's last move is recorded as Metronome (gChosenMove), which Mimic cannot copy.
    CHECK(sim->lastMoves[1] == MOVE_METRONOME, "last move of the metronome user is metronome (got %d)", sim->lastMoves[1]);
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "mimic failed");
    CHECK(B(0).moves[0] == MOVE_MIMIC, "mimic slot unchanged");
}

static int WantMirrorVsMetronome(struct BattleSim *sim)
{
    u16 called = CalledMove(sim, 1, 0, MOVE_METRONOME);
    // Metronome called a mirrorable attack that hit the player; on turn 2 the enemy was free to use Splash
    // (not locked into a multi-turn move, whose chosen move would be recorded) and the player used Mirror Move.
    return called != MOVE_NONE && (gBattleMoves[called].flags & FLAG_MIRROR_MOVE_AFFECTED) && gBattleMoves[called].power > 0
        && HP(0) < MAXHP(0) && LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_SPLASH, 1)
        && LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_MIRROR_MOVE, 1) && OUTCOME() == 0;
}
static void CheckMirrorVsMetronome(struct BattleSim *sim)
{
    // Only the originally chosen move (Metronome, not mirrorable) is considered for Mirror Move's record.
    CHECK(LOG_HAS_T(STRINGID_MIRRORMOVEFAILED, 1), "mirror move failed against a metronome-called attack");
}

// ===================== Sleep Talk =====================

static void CheckSleepTalkAwake(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "failed while awake");
    CHECK(PP(0, 0) == 9, "sleep talk pp deducted (pp %d)", PP(0, 0));
    CHECK(HP(1) == MAXHP(1), "no move called");
}

static void CheckSleepTalkCallsMove(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNFASTASLEEP), "asleep message");
    CHECK(CalledMove(sim, 0, 0, MOVE_SLEEP_TALK) == MOVE_TACKLE, "called tackle (got %d)", CalledMove(sim, 0, 0, MOVE_SLEEP_TALK));
    CHECK(HP(1) < MAXHP(1), "tackle hit");
    CHECK(PP(0, 0) == 9, "sleep talk pp deducted (pp %d)", PP(0, 0));
    CHECK(PP(0, 1) == 35, "called move's pp not deducted (pp %d)", PP(0, 1));
    CHECK(STATUS1(0) & STATUS1_SLEEP, "still asleep");
}

static void CheckSleepTalkZeroPp(struct BattleSim *sim)
{
    // Sleep Talk ignores the PP limitation when choosing, so the 0-PP Tackle is selected; the called move's own
    // attackcanceler then hits the no-PP check and it fails with "but there was no PP left".
    CHECK(CalledMove(sim, 0, 0, MOVE_SLEEP_TALK) == MOVE_TACKLE, "called the 0-pp tackle");
    CHECK(LOG_HAS(STRINGID_BUTNOPPLEFT), "no pp left message");
    CHECK(HP(1) == MAXHP(1), "tackle did not hit");
    CHECK(PP(0, 1) == 0 && PP(0, 0) == 9, "tackle stays at 0 pp, sleep talk pp used (%d %d)", PP(0, 1), PP(0, 0));
}

static void CheckSleepTalkNoValid(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNFASTASLEEP), "asleep");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "failed: no callable move");
    CHECK(CalledMove(sim, 0, 0, MOVE_SLEEP_TALK) == MOVE_NONE, "nothing called");
    CHECK(HP(1) == MAXHP(1), "no damage");
    CHECK(PP(0, 0) == 9, "sleep talk pp deducted (pp %d)", PP(0, 0));
}

static void CheckSleepTalkRest(struct BattleSim *sim)
{
    CHECK(CalledMove(sim, 0, 0, MOVE_SLEEP_TALK) == MOVE_REST, "called rest");
    CHECK(LOG_HAS(STRINGID_PKMNALREADYASLEEP2), "rest: already asleep");
    CHECK(PP(0, 1) == 10, "rest pp not deducted (pp %d)", PP(0, 1));
    CHECK(PP(0, 0) == 9, "sleep talk pp deducted (pp %d)", PP(0, 0));
}

static void CheckSleepTalkWakes(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNWOKEUP), "woke up this turn");
    CHECK(!(STATUS1(0) & STATUS1_SLEEP), "awake");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "sleep talk then fails (awake)");
    CHECK(HP(1) == MAXHP(1), "no move called");
}

static void CheckMimicCopiesSleepTalk(struct BattleSim *sim)
{
    // The sleeper's recorded last move is Sleep Talk (the chosen move), not the move it called.
    CHECK(CalledMove(sim, 1, 0, MOVE_SLEEP_TALK) == MOVE_TACKLE, "sleep talk called tackle");
    CHECK(sim->lastMoves[1] == MOVE_SLEEP_TALK, "last move recorded as sleep talk (got %d)", sim->lastMoves[1]);
    CHECK(B(0).moves[0] == MOVE_SLEEP_TALK, "mimic copied sleep talk (got %d)", B(0).moves[0]);
    CHECK(PP(0, 0) == 5, "5 pp");
}

// ===================== Assist =====================

static void CheckAssistCalls(struct BattleSim *sim)
{
    CHECK(CalledMove(sim, 0, 0, MOVE_ASSIST) == MOVE_TACKLE, "assist called the party mate's tackle (got %d)", CalledMove(sim, 0, 0, MOVE_ASSIST));
    CHECK(HP(1) < MAXHP(1), "tackle hit");
    CHECK(PP(0, 0) == 19, "assist pp deducted once (pp %d)", PP(0, 0));
    CHECK(B(0).moves[0] == MOVE_ASSIST, "moveset unchanged");
}

static void CheckAssistFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "assist failed");
    CHECK(CalledMove(sim, 0, 0, MOVE_ASSIST) == MOVE_NONE, "nothing called");
    CHECK(HP(1) == MAXHP(1), "no damage");
    CHECK(PP(0, 0) == 19, "assist pp deducted on failure (pp %d)", PP(0, 0));
}

// ===================== Mirror Move =====================

static void CheckMirrorMoveFirstTurn(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_MIRRORMOVEFAILED), "mirror move failed");
    CHECK(PP(0, 0) == 19, "pp deducted (pp %d)", PP(0, 0));
}

static void CheckMirrorMoveCopies(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "rattata tackled first");
    CHECK(CalledMove(sim, 0, 0, MOVE_MIRROR_MOVE) == MOVE_TACKLE, "mirror move became tackle (got %d)", CalledMove(sim, 0, 0, MOVE_MIRROR_MOVE));
    CHECK(HP(1) < MAXHP(1), "tackle hit the rattata");
    CHECK(PP(0, 0) == 19, "mirror move pp deducted once (pp %d)", PP(0, 0));
    CHECK(!LOG_HAS(STRINGID_MIRRORMOVEFAILED), "no failure message");
}

static void CheckMirrorMoveUncopyable(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_ROLE_PLAY, 0), "enemy used role play");
    CHECK(LOG_HAS(STRINGID_MIRRORMOVEFAILED), "role play cannot be mirrored");
}

static void CheckMirrorMoveAfterSwitch(struct BattleSim *sim)
{
    CHECK(B(1).species == SPECIES_PIDGEY, "enemy switched");
    CHECK(LOG_HAS_T(STRINGID_MIRRORMOVEFAILED, 1), "switching cleared the mirror move record");
}

// ===================== Mimic =====================

static void CheckMimicCopies(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNLEARNEDMOVE2), "learned message");
    CHECK(B(0).moves[0] == MOVE_TACKLE, "mimic slot now tackle (got %d)", B(0).moves[0]);
    CHECK(PP(0, 0) == 5, "5 pp (got %d)", PP(0, 0));
    CHECK(sim->disableStructs[0].mimickedMoves & 1, "mimicked bit set for slot 0");
    CHECK(GetMonData(&sim->playerParty[0], MON_DATA_MOVE1) == MOVE_MIMIC, "party data still has mimic");
}

static void CheckMimicLostOnSwitch(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_CHANSEY, "back in");
    CHECK(B(0).moves[0] == MOVE_MIMIC, "mimic restored after switching (got %d)", B(0).moves[0]);
    CHECK(PP(0, 0) == 9, "mimic's own pp use persisted, the copied move's use did not (pp %d)", PP(0, 0));
    CHECK(sim->disableStructs[0].mimickedMoves == 0, "mimicked bits cleared");
}

static void CheckMimicFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "mimic failed");
    CHECK(B(0).moves[0] == MOVE_MIMIC, "mimic slot unchanged");
    CHECK(PP(0, 0) == 9, "pp deducted (pp %d)", PP(0, 0));
}

static void CheckMimicFailsVsTransformed(struct BattleSim *sim)
{
    // Transform sets gChosenMove = MOVE_UNAVAILABLE, so the transformed mon's last move is unavailable to Mimic.
    CHECK(STATUS2(0) & STATUS2_TRANSFORMED, "ditto transformed");
    CHECK(MOVED_BEFORE(1, 0, 1), "enemy mimic went before the transformed ditto's counter");
    CHECK(Sc_LogCount(sim, STRINGID_BUTITFAILED, 1) == 2, "mimic failed (and the harmless counter failed too): %d failures", Sc_LogCount(sim, STRINGID_BUTITFAILED, 1));
    CHECK(B(1).moves[0] == MOVE_MIMIC, "mimic slot unchanged");
}

// ===================== Sketch =====================

static void CheckSketchPermanent(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSKETCHEDMOVE, 1), "sketched");
    CHECK(B(0).species == SPECIES_SMEARGLE, "smeargle back in");
    CHECK(B(0).moves[0] == MOVE_TACKLE, "tackle kept after switching (got %d)", B(0).moves[0]);
    CHECK(PP(0, 0) == 35, "full tackle pp (got %d)", PP(0, 0));
    CHECK(GetMonData(&sim->playerParty[0], MON_DATA_MOVE1) == MOVE_TACKLE, "party data changed permanently");
}

static void CheckSketchFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "sketch failed");
    CHECK(B(0).moves[0] == MOVE_SKETCH, "sketch slot unchanged");
    CHECK(PP(0, 0) == 0, "sketch's single pp used (pp %d)", PP(0, 0));
}

static void CheckSketchVsSketch(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_SKETCH, 0), "enemy sketched first");
    CHECK(LOG_COUNT(STRINGID_BUTITFAILED) == 2, "both sketches failed (%d failures)", LOG_COUNT(STRINGID_BUTITFAILED));
    CHECK(B(0).moves[0] == MOVE_SKETCH, "sketch slot unchanged");
}

static int WantSketched(struct BattleSim *sim) { return LOG_HAS(STRINGID_PKMNSKETCHEDMOVE) && OUTCOME() == 0; }
static void CheckSketchMetronome(struct BattleSim *sim)
{
    // Sketch copies the printed (chosen) move, so a Metronome user's last move is Metronome itself.
    CHECK(B(0).moves[0] == MOVE_METRONOME, "sketched metronome (got %d)", B(0).moves[0]);
    CHECK(PP(0, 0) == 10, "full metronome pp (got %d)", PP(0, 0));
}

// ===================== Transform =====================

static void CheckTransformCopies(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNTRANSFORMEDINTO), "transformed message");
    CHECK(STATUS2(0) & STATUS2_TRANSFORMED, "transformed flag");
    CHECK(B(0).species == SPECIES_SCYTHER, "species copied");
    CHECK(B(0).type1 == TYPE_BUG && B(0).type2 == TYPE_FLYING, "types copied (%d/%d)", B(0).type1, B(0).type2);
    CHECK(B(0).ability == ABILITY_SWARM, "ability copied");
    CHECK(B(0).attack == B(1).attack && B(0).defense == B(1).defense && B(0).speed == B(1).speed
          && B(0).spAttack == B(1).spAttack && B(0).spDefense == B(1).spDefense, "stats copied");
    CHECK(STAGE(0, STAT_ATK) == 8, "copied +2 attack stage (got %d)", STAGE(0, STAT_ATK));
    CHECK(B(0).moves[0] == MOVE_SWORDS_DANCE && B(0).moves[1] == MOVE_SLASH && B(0).moves[2] == MOVE_SPLASH && B(0).moves[3] == MOVE_SPLASH, "moves copied");
    CHECK(PP(0, 0) == 5 && PP(0, 1) == 5 && PP(0, 2) == 5 && PP(0, 3) == 5, "5 pp each (%d %d %d %d)", PP(0, 0), PP(0, 1), PP(0, 2), PP(0, 3));
    CHECK(HP(0) == MAXHP(0) && MAXHP(0) != MAXHP(1), "hp/max hp not copied");
    CHECK(B(0).item == ITEM_LEFTOVERS, "item kept");
    CHECK(B(0).level == 50, "level kept");
}

static void CheckDittoVsDitto(struct BattleSim *sim)
{
    int t0 = (STATUS2(0) & STATUS2_TRANSFORMED) != 0, t1 = (STATUS2(1) & STATUS2_TRANSFORMED) != 0;
    CHECK(LOG_COUNT(STRINGID_PKMNTRANSFORMEDINTO) == 1, "exactly one transform succeeded (%d)", LOG_COUNT(STRINGID_PKMNTRANSFORMEDINTO));
    CHECK(LOG_COUNT(STRINGID_BUTITFAILED) == 1, "the second one failed against a transformed target");
    CHECK(t0 + t1 == 1, "exactly one ditto is transformed");
    CHECK(B(0).species == SPECIES_DITTO && B(1).species == SPECIES_DITTO, "both still ditto");
}

static void CheckTransformFailsSemiInvulnerable(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNFLEWHIGH), "target flew up first");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "transform failed");
    CHECK(B(0).species == SPECIES_DITTO && !(STATUS2(0) & STATUS2_TRANSFORMED), "not transformed");
    CHECK(PP(0, 0) == 9, "transform pp deducted (pp %d)", PP(0, 0));
}

static void CheckTransformEndsOnSwitch(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNTRANSFORMEDINTO, 0), "transformed on turn 1");
    CHECK(B(0).species == SPECIES_DITTO, "ditto again after switching (got %s)", gSimSpeciesNames[B(0).species]);
    CHECK(!(STATUS2(0) & STATUS2_TRANSFORMED), "transform flag cleared");
    CHECK(B(0).moves[0] == MOVE_TRANSFORM && PP(0, 0) == 9, "transform back with its pp use persisted (pp %d)", PP(0, 0));
    CHECK(B(0).ability == ABILITY_LIMBER, "own ability back");
}

static void CheckTransformLowPpMove(struct BattleSim *sim)
{
    CHECK(B(0).species == SPECIES_SMEARGLE, "transformed into smeargle");
    CHECK(B(0).moves[0] == MOVE_SKETCH && PP(0, 0) == 1, "sketch copied with its 1 pp (pp %d)", PP(0, 0));
    CHECK(B(0).moves[1] == MOVE_TACKLE && PP(0, 1) == 5, "tackle copied with 5 pp (pp %d)", PP(0, 1));
}

static void CheckTransformVsSubstitute(struct BattleSim *sim)
{
    // The Gen 3 Transform script has no Substitute check.
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE, "target behind a substitute");
    CHECK(LOG_HAS_T(STRINGID_PKMNTRANSFORMEDINTO, 1), "transform still succeeds");
    CHECK(B(0).species == SPECIES_SNORLAX, "became snorlax");
}

// ===================== Conversion =====================

static void CheckConversionChanges(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNCHANGEDTYPE), "changed type");
    CHECK(B(0).type1 == TYPE_ELECTRIC && B(0).type2 == TYPE_ELECTRIC, "became electric (%d/%d)", B(0).type1, B(0).type2);
    CHECK(PP(0, 0) == 29, "pp deducted (pp %d)", PP(0, 0));
}

static void CheckConversionFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "conversion failed");
    CHECK(B(0).type1 == TYPE_NORMAL && B(0).type2 == TYPE_NORMAL, "still normal");
}

// ===================== Conversion 2 =====================

static void CheckConversion2Resists(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "hit first");
    CHECK(LOG_HAS(STRINGID_PKMNCHANGEDTYPE), "changed type");
    CHECK(B(0).type1 == B(0).type2, "both types set");
    CHECK(B(0).type1 == TYPE_ROCK || B(0).type1 == TYPE_STEEL || B(0).type1 == TYPE_GHOST, "a type resisting normal (got %d)", B(0).type1);
}

static void CheckConversion2NotHit(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "failed: not hit yet");
    CHECK(B(0).type1 == TYPE_NORMAL, "still normal");
}

static void CheckConversion2DragonVsSteel(struct BattleSim *sim)
{
    // The only type resisting Dragon is Steel, which Skarmory already is: the 1000 random tries all fail and
    // the fallback loop (which tests a stale index) either fails or "changes" the type to Steel again.
    CHECK(HP(0) == MAXHP(0) - 40, "hit by dragon rage (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(B(0).type1 == TYPE_STEEL && (B(0).type2 == TYPE_FLYING || B(0).type2 == TYPE_STEEL), "still steel (%d/%d)", B(0).type1, B(0).type2);
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1) || LOG_HAS_T(STRINGID_PKMNCHANGEDTYPE, 1), "either failed or re-set to steel");
    CHECK(!(LOG_HAS_T(STRINGID_PKMNCHANGEDTYPE, 1) && B(0).type2 == TYPE_FLYING), "if it 'changed', flying was overwritten by steel");
}

// ===================== Role Play / Skill Swap =====================

static void CheckRolePlay(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNCOPIEDFOE), "copied message");
    CHECK(B(0).ability == ABILITY_GUTS, "copied guts (got %d)", B(0).ability);
    CHECK(B(1).ability == ABILITY_GUTS, "target keeps its ability");
}

static void CheckAbilityMoveFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "failed");
    CHECK(B(0).ability == ABILITY_SYNCHRONIZE, "user ability unchanged (got %d)", B(0).ability);
    CHECK(B(1).ability == ABILITY_WONDER_GUARD, "wonder guard unchanged");
}

static void CheckSkillSwapUserWonderGuard(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "failed");
    CHECK(B(0).ability == ABILITY_WONDER_GUARD && B(1).ability == ABILITY_RUN_AWAY, "abilities unchanged");
}

static void CheckRolePlayIntimidate(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 5, "intimidated at the start (stage %d)", STAGE(0, STAT_ATK));
    CHECK(B(0).ability == ABILITY_INTIMIDATE, "copied intimidate");
    CHECK(STAGE(1, STAT_ATK) == 6, "copied intimidate did not activate (stage %d)", STAGE(1, STAT_ATK));
}

static void CheckSkillSwap(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSWAPPEDABILITIES), "swapped message");
    CHECK(B(0).ability == ABILITY_GUTS, "user got guts (got %d)", B(0).ability);
    CHECK(B(1).ability == ABILITY_SYNCHRONIZE, "target got synchronize (got %d)", B(1).ability);
}

static void CheckSkillSwapIntimidate(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 5, "intimidated at the start (stage %d)", STAGE(0, STAT_ATK));
    CHECK(B(0).ability == ABILITY_INTIMIDATE, "user now has intimidate");
    CHECK(B(1).species == SPECIES_RATTATA, "enemy switched to rattata");
    CHECK(STAGE(1, STAT_ATK) == 6, "swapped intimidate did not trigger on the switch-in (stage %d)", STAGE(1, STAT_ATK));
}

// ===================== Trick =====================

static void CheckTrickBoth(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSWITCHEDITEMS) && LOG_HAS(STRINGID_PKMNOBTAINEDXYOBTAINEDZ), "swap messages");
    CHECK(B(0).item == ITEM_LEFTOVERS, "user got leftovers (got %d)", B(0).item);
    CHECK(B(1).item == ITEM_CHOICE_BAND, "target got choice band (got %d)", B(1).item);
    CHECK(sim->sBattleStructStorage.choicedMove[0] == MOVE_NONE, "user not choice-locked into trick");
}

static void CheckTrickTaken(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNOBTAINEDX), "obtained message");
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_NONE, "user took the item");
}

static void CheckTrickGiven(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNOBTAINEDX2), "target obtained message");
    CHECK(B(0).item == ITEM_NONE && B(1).item == ITEM_LEFTOVERS, "user gave the item");
}

static void CheckTrickFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "trick failed");
    CHECK(!LOG_HAS(STRINGID_PKMNSWITCHEDITEMS), "no swap");
}

static void CheckTrickStickyHold(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEYINEFFECTIVE), "sticky hold message");
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_ORAN_BERRY, "items unchanged");
}

static void CheckTrickOpponentFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_TRICK), "enemy used trick");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "opponent's trick fails in a regular trainer battle");
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_CHOICE_BAND, "items unchanged");
}

static void CheckTrickChoiceLock(struct BattleSim *sim)
{
    // Trick goes first; Snorlax's Tackle the same turn is used while holding the band and locks it in.
    // (The opponent's lock is only honoured by the AI's move choice, not enforced by the engine, so only
    // the recorded lock is checked here; player-side enforcement is covered by the next scenario.)
    CHECK(B(1).item == ITEM_CHOICE_BAND, "snorlax holds the choice band");
    CHECK(sim->sBattleStructStorage.choicedMove[1] == MOVE_TACKLE, "locked into tackle (got %d)", sim->sBattleStructStorage.choicedMove[1]);
    CHECK(sim->sBattleStructStorage.choicedMove[0] == MOVE_NONE, "trick user not locked");
}

static void CheckTrickTakenBandLocks(struct BattleSim *sim)
{
    // The band taken by Trick is only applied at MOVEEND_CHANGED_ITEMS (after the choice-lock state), so the
    // Trick user is not locked into Trick; its next move (Confusion) locks it, and Splash cannot be chosen after.
    CHECK(B(0).item == ITEM_CHOICE_BAND && B(1).item == ITEM_NONE, "took the choice band");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_CONFUSION, 1), "confusion used freely on turn 2");
    CHECK(sim->sBattleStructStorage.choicedMove[0] == MOVE_CONFUSION, "locked into confusion (got %d)", sim->sBattleStructStorage.choicedMove[0]);
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_CONFUSION, 2) && !LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_SPLASH, 2), "could not pick splash on turn 3");
}

static void CheckTrickAfterKnockOff(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNKNOCKEDOFF, 0), "item knocked off on turn 1");
    CHECK(sim->wishFutureKnock.knockedOffMons[B_SIDE_PLAYER] & 1, "knocked-off flag set");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "trick failed for a knocked-off mon");
    CHECK(B(0).item == ITEM_NONE && B(1).item == ITEM_LEFTOVERS, "items unchanged");
}

// ===================== Recycle =====================

static void CheckRecycleCheri(struct BattleSim *sim)
{
    CHECK(!(STATUS1(0) & STATUS1_PARALYSIS), "cheri berry cured paralysis at the start");
    CHECK(LOG_HAS(STRINGID_XFOUNDONEY), "recycle message");
    CHECK(B(0).item == ITEM_CHERI_BERRY, "cheri berry restored (got %d)", B(0).item);
    CHECK(sim->sBattleStructStorage.usedHeldItems[0] == ITEM_NONE, "used item slot cleared");
}

static void CheckRecycleFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_RECYCLE), "used recycle");
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "recycle failed");
    CHECK(B(0).item == ITEM_NONE, "still no item");
}

static void CheckRecycleAcrossSwitch(struct BattleSim *sim)
{
    // usedHeldItems is per battler slot and not cleared on switching: the replacement recycles the berry.
    CHECK(PARTY_STATUS(B_SIDE_PLAYER, 0) == 0, "snorlax's paralysis was cured by the berry on turn 1");
    CHECK(B(0).species == SPECIES_CHANSEY, "chansey in");
    CHECK(LOG_HAS(STRINGID_XFOUNDONEY), "recycle message");
    CHECK(B(0).item == ITEM_CHERI_BERRY, "chansey got snorlax's cheri berry (got %d)", B(0).item);
}

// ===================== Knock Off =====================

static void CheckKnockOff(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNKNOCKEDOFF), "knocked off message");
    CHECK(B(1).item == ITEM_NONE, "item removed");
    CHECK(sim->wishFutureKnock.knockedOffMons[B_SIDE_OPPONENT] & 1, "knocked-off flag");
    CHECK(sim->sBattleStructStorage.choicedMove[1] == MOVE_NONE, "choice lock cleared");
    CHECK(HP(1) < MAXHP(1), "damage dealt");
}

static void CheckKnockOffStickyHold(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEYINEFFECTIVE), "sticky hold message");
    CHECK(B(1).item == ITEM_ORAN_BERRY, "item kept");
    CHECK(!LOG_HAS(STRINGID_PKMNKNOCKEDOFF), "no knock off message");
}

static void CheckKnockOffKo(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNKNOCKEDOFF), "no knock off on a fainting target");
    CHECK(GetMonData(&sim->enemyParty[0], MON_DATA_HELD_ITEM) == ITEM_LEFTOVERS, "fainted mon keeps its item");
    CHECK(!(sim->wishFutureKnock.knockedOffMons[B_SIDE_OPPONENT] & 1), "no knocked-off flag");
}

static void CheckKnockOffSubstitute(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNKNOCKEDOFF), "substitute blocked knock off");
    CHECK(B(1).item == ITEM_LEFTOVERS, "item kept");
}

// ===================== Thief / Covet =====================

static void CheckThiefSteals(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSTOLEITEM), "stole message");
    CHECK(B(0).item == ITEM_ORAN_BERRY, "user holds the berry (got %d)", B(0).item);
    CHECK(B(1).item == ITEM_NONE, "target lost it");
}

static void CheckThiefNoSteal(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNSTOLEITEM), "nothing stolen");
    CHECK(B(1).item == ITEM_ORAN_BERRY, "target keeps the berry");
}

static void CheckThiefOpponentFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE(STRINGID_USEDMOVE, MOVE_THIEF) && HP(0) < MAXHP(0), "enemy thief hit");
    CHECK(!LOG_HAS(STRINGID_PKMNSTOLEITEM), "opponent cannot steal in a regular trainer battle");
    CHECK(B(0).item == ITEM_ORAN_BERRY && B(1).item == ITEM_NONE, "items unchanged");
}

static void CheckThiefMail(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNSTOLEITEM), "mail cannot be stolen");
    CHECK(B(0).item == ITEM_NONE && B(1).item == ITEM_ORANGE_MAIL, "items unchanged");
}

static void CheckThiefUserHolding(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNSTOLEITEM), "nothing stolen");
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_ORAN_BERRY, "items unchanged");
}

static void CheckThiefStickyHold(struct BattleSim *sim)
{
    // The Sticky Hold check comes before the "user already holds an item" check.
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEYINEFFECTIVE), "sticky hold message even though the user holds an item");
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_ORAN_BERRY, "items unchanged");
}

static void CheckThiefKo(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSTOLEITEM), "stole from the fainting target");
    CHECK(B(0).item == ITEM_LEFTOVERS, "user holds the stolen item");
    CHECK(GetMonData(&sim->enemyParty[0], MON_DATA_HELD_ITEM) == ITEM_NONE, "fainted mon lost it");
}

static void CheckThiefAfterKnockOff(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNKNOCKEDOFF, 0), "user's item knocked off");
    CHECK(!LOG_HAS(STRINGID_PKMNSTOLEITEM), "a knocked-off mon cannot steal");
    CHECK(B(0).item == ITEM_NONE && B(1).item == ITEM_ORAN_BERRY, "items unchanged");
}

// ===================== Nature Power / Secret Power / Camouflage =====================

static int WantParalyzed(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_PARALYSIS) != 0; }
static void CheckNaturePower(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_NATUREPOWERTURNEDINTO), "nature power message");
    CHECK(CalledMove(sim, 0, 0, MOVE_NATURE_POWER) == MOVE_STUN_SPORE, "grass terrain: stun spore (got %d)", CalledMove(sim, 0, 0, MOVE_NATURE_POWER));
    CHECK(STATUS1(1) & STATUS1_PARALYSIS, "paralyzed");
    CHECK(PP(0, 0) == 19, "nature power pp deducted once (pp %d)", PP(0, 0));
}

static int WantPoisoned(struct BattleSim *sim) { return (STATUS1(1) & STATUS1_POISON) != 0; }
static void CheckSecretPower(struct BattleSim *sim)
{
    CHECK(HP(1) < MAXHP(1), "hit");
    CHECK(STATUS1(1) & STATUS1_POISON, "grass terrain: poison side effect");
    CHECK(LOG_HAS(STRINGID_PKMNWASPOISONED), "poisoned message");
}

static int WantSecretPowerNoEffect(struct BattleSim *sim) { return HP(1) < MAXHP(1) && STATUS1(1) == 0; }
static void CheckSecretPowerNoEffect(struct BattleSim *sim)
{
    CHECK(STATUS1(1) == 0, "no side effect this time");
    CHECK(!(STATUS1(1) & (STATUS1_PARALYSIS | STATUS1_SLEEP)), "never another terrain's effect");
}

static void CheckCamouflage(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNCHANGEDTYPE), "changed type");
    CHECK(B(0).type1 == TYPE_GRASS && B(0).type2 == TYPE_GRASS, "grass terrain: grass type (%d/%d)", B(0).type1, B(0).type2);
}

static void CheckCamouflageFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "already grass: fails");
    CHECK(B(0).type1 == TYPE_GRASS && B(0).type2 == TYPE_POISON, "types unchanged");
}

// ===================== scenarios =====================

#define SPLASH4 MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH
#define MON_S(sp, lv) { .species = sp, .level = lv, .moves = { SPLASH4 } }
#define MON_S_ITEM(sp, lv, it) { .species = sp, .level = lv, .moves = { SPLASH4 }, .item = it }
#define MON_S_AB(sp, lv, ab) { .species = sp, .level = lv, .moves = { SPLASH4 }, .abilityNum = ab }
#define ENEMY_SNORLAX MON_S(SPECIES_SNORLAX, 50)
#define ENEMY_STEELIX MON_S(SPECIES_STEELIX, 50)

static const struct Scenario sScenarios[] =
{
    // --- Metronome ---
    { .name = "metronome_calls_allowed_move",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_METRONOME, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_STEELIX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMetronomeBasic },
    { .name = "metronome_two_turn_move",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_METRONOME, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_STEELIX },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .wantSeed = WantMetronomeTwoTurn, .check = CheckMetronomeTwoTurn },
    { .name = "mimic_fails_vs_metronome",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIMIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLEFABLE, 50, MOVE_METRONOME, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .wantSeed = WantMimicVsMetronome, .check = CheckMimicVsMetronome },
    { .name = "mirror_move_fails_vs_metronome",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MIRROR_MOVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_CLEFABLE, 50, MOVE_METRONOME, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = WantMirrorVsMetronome, .check = CheckMirrorVsMetronome },

    // --- Sleep Talk ---
    { .name = "sleep_talk_awake_fails",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_SLEEP_TALK, MOVE_TACKLE, MOVE_NONE, MOVE_NONE) },
      .enemy = { ENEMY_STEELIX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkAwake },
    { .name = "sleep_talk_calls_move_no_pp_use",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SLEEP_TALK, MOVE_TACKLE, MOVE_NONE, MOVE_NONE }, .status = STATUS1_SLEEP_TURN(4) } },
      .enemy = { ENEMY_STEELIX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkCallsMove },
    { .name = "sleep_talk_calls_zero_pp_move",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SLEEP_TALK, MOVE_TACKLE, MOVE_NONE, MOVE_NONE }, .status = STATUS1_SLEEP_TURN(4), .pp = { 10, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { ENEMY_STEELIX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkZeroPp },
    { .name = "sleep_talk_no_two_turn_or_focus_punch",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SLEEP_TALK, MOVE_FLY, MOVE_SOLAR_BEAM, MOVE_FOCUS_PUNCH }, .status = STATUS1_SLEEP_TURN(4) } },
      .enemy = { ENEMY_STEELIX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkNoValid },
    { .name = "sleep_talk_no_calling_moves",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SLEEP_TALK, MOVE_METRONOME, MOVE_UPROAR, MOVE_MIRROR_MOVE }, .status = STATUS1_SLEEP_TURN(4) } },
      .enemy = { ENEMY_STEELIX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkNoValid },
    { .name = "sleep_talk_rest_already_asleep",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SLEEP_TALK, MOVE_REST, MOVE_NONE, MOVE_NONE }, .status = STATUS1_SLEEP_TURN(4) } },
      .enemy = { ENEMY_STEELIX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkRest },
    { .name = "sleep_talk_wakes_up_then_fails",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SLEEP_TALK, MOVE_TACKLE, MOVE_NONE, MOVE_NONE }, .status = STATUS1_SLEEP_TURN(1) } },
      .enemy = { ENEMY_STEELIX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSleepTalkWakes },
    { .name = "mimic_copies_sleep_talk_not_called_move",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIMIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SLEEP_TALK, MOVE_TACKLE, MOVE_NONE, MOVE_NONE }, .status = STATUS1_SLEEP_TURN(5) } },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .check = CheckMimicCopiesSleepTalk },

    // --- Assist ---
    { .name = "assist_calls_party_move",
      .player = { MON(SPECIES_SKITTY, 50, MOVE_ASSIST, MOVE_NONE, MOVE_NONE, MOVE_NONE), MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAssistCalls },
    { .name = "assist_no_party_fails",
      .player = { MON(SPECIES_SKITTY, 50, MOVE_ASSIST, MOVE_TACKLE, MOVE_NONE, MOVE_NONE) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAssistFails },
    { .name = "assist_forbidden_moves_fail",
      .player = { MON(SPECIES_SKITTY, 50, MOVE_ASSIST, MOVE_NONE, MOVE_NONE, MOVE_NONE),
                  MON(SPECIES_RATTATA, 50, MOVE_PROTECT, MOVE_COUNTER, MOVE_THIEF, MOVE_TRICK),
                  MON(SPECIES_RATTATA, 50, MOVE_METRONOME, MOVE_SLEEP_TALK, MOVE_FOCUS_PUNCH, MOVE_COVET),
                  MON(SPECIES_RATTATA, 50, MOVE_MIRROR_MOVE, MOVE_ASSIST, MOVE_HELPING_HAND, MOVE_ENDURE) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAssistFails },
    { .name = "assist_ignores_own_moves",
      .player = { MON(SPECIES_SKITTY, 50, MOVE_ASSIST, MOVE_TACKLE, MOVE_SCRATCH, MOVE_GROWL), MON(SPECIES_RATTATA, 50, MOVE_DETECT, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAssistFails },
    { .name = "assist_uses_fainted_party_member",
      .player = { MON(SPECIES_SKITTY, 50, MOVE_ASSIST, MOVE_NONE, MOVE_NONE, MOVE_NONE), { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .hp = 0, .hpSet = 1 } },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAssistCalls },

    // --- Mirror Move ---
    { .name = "mirror_move_first_turn_fails",
      .player = { MON(SPECIES_PIDGEOT, 50, MOVE_MIRROR_MOVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMirrorMoveFirstTurn },
    { .name = "mirror_move_copies_tackle",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MIRROR_MOVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMirrorMoveCopies },
    { .name = "mirror_move_uncopyable_move",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MIRROR_MOVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_ROLE_PLAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMirrorMoveUncopyable },
    { .name = "mirror_move_fails_after_switch",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_MIRROR_MOVE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON_S(SPECIES_PIDGEY, 50) },
      .actions = { T(1, 0), T(0, SC_SWITCH(1)) }, .turns = 2, .check = CheckMirrorMoveAfterSwitch },

    // --- Mimic ---
    { .name = "mimic_copies_with_5_pp",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIMIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .check = CheckMimicCopies },
    { .name = "mimic_lost_on_switch",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIMIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH), MON_S(SPECIES_SNORLAX, 50) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 0), T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0) }, .turns = 5, .check = CheckMimicLostOnSwitch },
    { .name = "mimic_fails_before_target_moved",
      .player = { MON(SPECIES_JOLTEON, 50, MOVE_MIMIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMimicFails },
    { .name = "mimic_fails_vs_struggle",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIMIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .check = CheckMimicFails },
    { .name = "mimic_fails_known_move",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIMIC, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(2, 0), T(0, 1) }, .turns = 2, .check = CheckMimicFails },
    { .name = "mimic_fails_vs_substitute",
      .player = { MON(SPECIES_CHANSEY, 50, MOVE_MIMIC, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckMimicFails },
    { .name = "mimic_fails_vs_transformed",
      .player = { MON(SPECIES_DITTO, 50, MOVE_TRANSFORM, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .enemy = { MON(SPECIES_CHANSEY, 50, MOVE_MIMIC, MOVE_COUNTER, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 2), T(1, 0) }, .turns = 2, .check = CheckMimicFailsVsTransformed },

    // --- Sketch ---
    { .name = "sketch_permanent_copy",
      .player = { MON(SPECIES_SMEARGLE, 50, MOVE_SKETCH, MOVE_SPLASH, MOVE_NONE, MOVE_NONE), MON_S(SPECIES_SNORLAX, 50) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1), T(SC_SWITCH(1), 1), T(SC_SWITCH(0), 1) }, .turns = 4, .check = CheckSketchPermanent },
    { .name = "sketch_fails_vs_struggle",
      .player = { MON(SPECIES_SMEARGLE, 50, MOVE_SKETCH, MOVE_SPLASH, MOVE_NONE, MOVE_NONE) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_NONE, MOVE_NONE, MOVE_NONE }, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .check = CheckSketchFails },
    { .name = "sketch_fails_vs_sketch",
      .player = { MON(SPECIES_SMEARGLE, 50, MOVE_SKETCH, MOVE_SPLASH, MOVE_NONE, MOVE_NONE) },
      .enemy = { MON(SPECIES_SMEARGLE, 50, MOVE_SKETCH, MOVE_SPLASH, MOVE_NONE, MOVE_NONE) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckSketchVsSketch },
    { .name = "sketch_fails_known_move",
      .player = { MON(SPECIES_SMEARGLE, 50, MOVE_SKETCH, MOVE_TACKLE, MOVE_SPLASH, MOVE_NONE) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(2, 0), T(0, 1) }, .turns = 2, .check = CheckSketchFails },
    { .name = "sketch_fails_vs_substitute",
      .player = { MON(SPECIES_SMEARGLE, 50, MOVE_SKETCH, MOVE_SPLASH, MOVE_NONE, MOVE_NONE) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckSketchFails },
    { .name = "sketch_copies_metronome_itself",
      .player = { MON(SPECIES_SMEARGLE, 50, MOVE_SKETCH, MOVE_SPLASH, MOVE_NONE, MOVE_NONE) },
      .enemy = { MON(SPECIES_CLEFABLE, 50, MOVE_METRONOME, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .wantSeed = WantSketched, .check = CheckSketchMetronome },

    // --- Transform ---
    { .name = "transform_copies_stats_stages_moves",
      .player = { MON_ITEM(SPECIES_DITTO, 50, MOVE_TRANSFORM, MOVE_NONE, MOVE_NONE, MOVE_NONE, ITEM_LEFTOVERS) },
      .enemy = { MON(SPECIES_SCYTHER, 50, MOVE_SWORDS_DANCE, MOVE_SLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTransformCopies },
    { .name = "transform_ditto_vs_ditto",
      .player = { MON(SPECIES_DITTO, 50, MOVE_TRANSFORM, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .enemy = { MON(SPECIES_DITTO, 50, MOVE_TRANSFORM, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckDittoVsDitto },
    { .name = "transform_fails_vs_flying_target",
      .player = { MON(SPECIES_DITTO, 50, MOVE_TRANSFORM, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .enemy = { MON(SPECIES_PIDGEOT, 50, MOVE_FLY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTransformFailsSemiInvulnerable },
    { .name = "transform_ends_on_switch",
      .player = { MON(SPECIES_DITTO, 50, MOVE_TRANSFORM, MOVE_NONE, MOVE_NONE, MOVE_NONE), MON_S(SPECIES_SNORLAX, 50) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 1), T(SC_SWITCH(1), 1), T(SC_SWITCH(0), 1) }, .turns = 3, .check = CheckTransformEndsOnSwitch },
    { .name = "transform_copies_low_pp_move",
      .player = { MON(SPECIES_DITTO, 50, MOVE_TRANSFORM, MOVE_NONE, MOVE_NONE, MOVE_NONE) },
      .enemy = { MON(SPECIES_SMEARGLE, 50, MOVE_SKETCH, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 2) }, .turns = 1, .check = CheckTransformLowPpMove },
    { .name = "transform_vs_substitute_succeeds",
      .player = { MON(SPECIES_DITTO, 50, MOVE_TRANSFORM, MOVE_SPLASH, MOVE_NONE, MOVE_NONE) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckTransformVsSubstitute },

    // --- Conversion ---
    { .name = "conversion_changes_to_move_type",
      .player = { MON(SPECIES_PORYGON, 50, MOVE_CONVERSION, MOVE_THUNDERBOLT, MOVE_NONE, MOVE_NONE) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckConversionChanges },
    { .name = "conversion_fails_all_own_type",
      .player = { MON(SPECIES_PORYGON, 50, MOVE_CONVERSION, MOVE_TACKLE, MOVE_SPLASH, MOVE_SHARPEN) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckConversionFails },
    { .name = "conversion_mystery_type_counts_as_normal",
      .player = { MON(SPECIES_PORYGON, 50, MOVE_CONVERSION, MOVE_CURSE, MOVE_NONE, MOVE_NONE) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckConversionFails },
    { .name = "conversion_stops_at_empty_slot",
      .player = { MON(SPECIES_PORYGON, 50, MOVE_CONVERSION, MOVE_NONE, MOVE_THUNDERBOLT, MOVE_NONE) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckConversionFails },

    // --- Conversion 2 ---
    { .name = "conversion2_resists_last_hit",
      .player = { MON(SPECIES_PORYGON, 50, MOVE_CONVERSION_2, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckConversion2Resists },
    { .name = "conversion2_fails_not_hit",
      .player = { MON(SPECIES_PORYGON, 50, MOVE_CONVERSION_2, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckConversion2NotHit },
    { .name = "conversion2_dragon_hit_on_steel_type",
      .player = { MON(SPECIES_SKARMORY, 50, MOVE_CONVERSION_2, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON(SPECIES_DRATINI, 50, MOVE_DRAGON_RAGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckConversion2DragonVsSteel },

    // --- Role Play / Skill Swap ---
    { .name = "role_play_copies_ability",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_ROLE_PLAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S(SPECIES_MACHAMP, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRolePlay },
    { .name = "role_play_fails_wonder_guard",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_ROLE_PLAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S(SPECIES_SHEDINJA, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAbilityMoveFails },
    { .name = "role_play_intimidate_no_trigger",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_ROLE_PLAY, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S(SPECIES_GYARADOS, 50) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckRolePlayIntimidate },
    { .name = "skill_swap_swaps",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_SKILL_SWAP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S(SPECIES_MACHAMP, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSkillSwap },
    { .name = "skill_swap_fails_target_wonder_guard",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_SKILL_SWAP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S(SPECIES_SHEDINJA, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAbilityMoveFails },
    { .name = "skill_swap_fails_user_wonder_guard",
      .player = { MON(SPECIES_SHEDINJA, 50, MOVE_SKILL_SWAP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S(SPECIES_RATTATA, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSkillSwapUserWonderGuard },
    { .name = "skill_swap_intimidate_no_retrigger",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_SKILL_SWAP, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S(SPECIES_GYARADOS, 50), MON_S(SPECIES_RATTATA, 50) },
      .actions = { T(0, 0), T(1, SC_SWITCH(1)), T(1, 0) }, .turns = 3, .check = CheckSkillSwapIntimidate },

    // --- Trick ---
    { .name = "trick_swaps_both_items",
      .player = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND) },
      .enemy = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickBoth },
    { .name = "trick_takes_item",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickTaken },
    { .name = "trick_gives_item",
      .player = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickGiven },
    { .name = "trick_fails_no_items",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickFails },
    { .name = "trick_fails_sticky_hold",
      .player = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { { .species = SPECIES_GULPIN, .level = 50, .moves = { SPLASH4 }, .item = ITEM_ORAN_BERRY, .abilityNum = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickStickyHold },
    { .name = "trick_fails_enigma_berry",
      .player = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_ENIGMA_BERRY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickFails },
    { .name = "trick_fails_mail",
      .player = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_ORANGE_MAIL) },
      .enemy = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickFails },
    { .name = "trick_by_opponent_fails",
      .player = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_LEFTOVERS) },
      .enemy = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickOpponentFails },
    { .name = "trick_choice_band_locks_receiver",
      .player = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckTrickChoiceLock },
    { .name = "trick_taken_choice_band_locks_next_move",
      .player = { MON(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_CONFUSION, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_CHOICE_BAND) },
      .actions = { T(0, 0), T(1, 0), T(2, 0) }, .turns = 3, .check = CheckTrickTakenBandLocks },
    { .name = "trick_fails_after_knock_off",
      .player = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND) },
      .enemy = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckTrickAfterKnockOff },

    // --- Recycle ---
    { .name = "recycle_restores_used_berry",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_RECYCLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH }, .item = ITEM_CHERI_BERRY, .status = STATUS1_PARALYSIS } },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRecycleCheri },
    { .name = "recycle_fails_nothing_used",
      .player = { MON(SPECIES_SNORLAX, 50, MOVE_RECYCLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRecycleFails },
    { .name = "recycle_fails_after_knock_off",
      .player = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_RECYCLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHERI_BERRY) },
      .enemy = { MON(SPECIES_RATTATA, 50, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckRecycleFails },
    { .name = "recycle_slot_survives_switch",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { SPLASH4 }, .item = ITEM_CHERI_BERRY, .status = STATUS1_PARALYSIS },
                  MON(SPECIES_CHANSEY, 50, MOVE_RECYCLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(1, 0), T(SC_SWITCH(1), 0), T(0, 0) }, .turns = 3, .check = CheckRecycleAcrossSwitch },

    // --- Knock Off ---
    { .name = "knock_off_removes_item",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_TACKLE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .check = CheckKnockOff },
    { .name = "knock_off_sticky_hold",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_GULPIN, .level = 50, .moves = { SPLASH4 }, .item = ITEM_ORAN_BERRY, .abilityNum = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckKnockOffStickyHold },
    { .name = "knock_off_no_effect_on_ko",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { SPLASH4 }, .item = ITEM_LEFTOVERS, .hp = 1, .hpSet = 1 }, MON_S(SPECIES_PIDGEY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckKnockOffKo },
    { .name = "knock_off_blocked_by_substitute",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckKnockOffSubstitute },

    // --- Thief / Covet ---
    { .name = "thief_steals_item",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_ORAN_BERRY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefSteals },
    { .name = "covet_steals_item",
      .player = { MON(SPECIES_SKITTY, 50, MOVE_COVET, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_ORAN_BERRY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefSteals },
    { .name = "thief_fails_user_holding_item",
      .player = { MON_ITEM(SPECIES_SNEASEL, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_ORAN_BERRY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefUserHolding },
    { .name = "thief_sticky_hold_before_holding_check",
      .player = { MON_ITEM(SPECIES_SNEASEL, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { { .species = SPECIES_GULPIN, .level = 50, .moves = { SPLASH4 }, .item = ITEM_ORAN_BERRY, .abilityNum = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefStickyHold },
    { .name = "thief_blocked_by_substitute",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_SUBSTITUTE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_ORAN_BERRY) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckThiefNoSteal },
    { .name = "thief_by_opponent_fails",
      .player = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_ORAN_BERRY) },
      .enemy = { MON(SPECIES_SNEASEL, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefOpponentFails },
    { .name = "thief_cannot_steal_mail",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S_ITEM(SPECIES_SNORLAX, 50, ITEM_ORANGE_MAIL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefMail },
    { .name = "thief_steals_on_ko",
      .player = { MON(SPECIES_SNEASEL, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { { .species = SPECIES_RATTATA, .level = 50, .moves = { SPLASH4 }, .item = ITEM_LEFTOVERS, .hp = 1, .hpSet = 1 }, MON_S(SPECIES_PIDGEY, 50) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefKo },
    { .name = "thief_fails_after_own_item_knocked_off",
      .player = { MON_ITEM(SPECIES_SNEASEL, 50, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { MON_ITEM(SPECIES_SNORLAX, 50, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_ORAN_BERRY) },
      .actions = { T(1, 0), T(0, 1) }, .turns = 2, .check = CheckThiefAfterKnockOff },

    // --- Nature Power / Secret Power / Camouflage (grass terrain) ---
    { .name = "nature_power_grass_stun_spore",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_NATURE_POWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantParalyzed, .check = CheckNaturePower },
    { .name = "secret_power_grass_poison",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_SECRET_POWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S_AB(SPECIES_SNORLAX, 50, 1) }, // Thick Fat, not Immunity
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantPoisoned, .check = CheckSecretPower },
    { .name = "secret_power_no_effect_branch",
      .player = { MON(SPECIES_CLEFABLE, 50, MOVE_SECRET_POWER, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { MON_S_AB(SPECIES_SNORLAX, 50, 1) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSecretPowerNoEffect, .check = CheckSecretPowerNoEffect },
    { .name = "camouflage_grass",
      .player = { MON(SPECIES_KECLEON, 50, MOVE_CAMOUFLAGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCamouflage },
    { .name = "camouflage_fails_already_grass",
      .player = { MON(SPECIES_BULBASAUR, 50, MOVE_CAMOUFLAGE, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .enemy = { ENEMY_SNORLAX },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCamouflageFails },
};

SCENARIO_GROUP(copy_random, sScenarios)
