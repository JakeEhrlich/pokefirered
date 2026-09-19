// Held items: HP/status/stat berries, Leftovers, Choice Band, Quick Claw, King's Rock, Focus Band,
// Shell Bell, accuracy items, type-boost and species-specific boost items, herbs, and the moves that
// remove, steal, swap or recycle items.
//
// Timing recap (battle_util.c ItemBattleEffects):
//  - ITEMEFFECT_ON_SWITCH_IN (battle start / switch-in, after Intimidate & Trace): Amulet Coin, White Herb.
//  - ITEMEFFECT_MOVE_END (Cmd_moveend, after EVERY move, all battlers): status-cure berries, Persim,
//    Lum, Mental Herb, White Herb.
//  - ITEMEFFECT_NORMAL at end of turn, per battler in turn order, ENDTURN_ITEMS1 (moveTurn = FALSE):
//    HP berries / Berry Juice (<= 1/2), Leppa, Leftovers, Figy family, stat berries (<= 1/4), Lansat,
//    Starf, status cures; ENDTURN_ITEMS2 (moveTurn = TRUE): status cures again (after Yawn etc.).
//    Ingrain runs before ITEMS1; Leech Seed, poison, burn... run after it.
//  - ITEMEFFECT_KINGSROCK_SHELLBELL (moveend): King's Rock (10 %, flagged moves only), Shell Bell (1/8).
//  - Damage-time: type boosts / Choice Band / Soul Dew / Deep Sea Tooth+Scale / Light Ball / Thick Club /
//    Metal Powder (CalculateBaseDamage), Focus Band (adjustnormaldamage), Bright Powder / Lax Incense
//    (accuracycheck), Scope Lens / Lucky Punch / Stick (critcalc), Quick Claw / Macho Brace (turn order).
#include "scenario.h"

extern const u8 gTypeEffectiveness[336];
#define TE_FORESIGHT 0xFE
#define TE_ENDTABLE  0xFF

#define SPLASH3 MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH
#define M1(sp, lv, m) { .species = sp, .level = lv, .moves = { m, SPLASH3 } }
#define MI(sp, lv, m, it) { .species = sp, .level = lv, .moves = { m, SPLASH3 }, .item = it }
#define MHP(sp, lv, m, it, h) { .species = sp, .level = lv, .moves = { m, SPLASH3 }, .item = it, .hp = h, .hpSet = 1 }
#define MHPN(sp, lv, m, it, h, nat) { .species = sp, .level = lv, .moves = { m, SPLASH3 }, .item = it, .hp = h, .hpSet = 1, .nature = nat }
#define PARTY_ITEM(side, i) (GetMonData(&(side == B_SIDE_PLAYER ? sim->playerParty : sim->enemyParty)[i], MON_DATA_HELD_ITEM))
#define CHOICED(b) (sim->sBattleStructStorage.choicedMove[b])

// ---------------------------------------------------------------------------------------------
// Damage helpers. All damage scenarios: battler 0 (player) attacks battler 1 (enemy, using Splash)
// once on turn 0, both start at full HP.
static int FirstMoveOf(struct BattleSim *sim, u8 battler)
{
    int i = Sc_LogIndex(sim, STRINGID_USEDMOVE, battler, 0, 0);
    return i >= 0 ? sim->log[i].move : MOVE_NONE;
}

// One non-critical hit of `move` by battler atk on def with the current battle mons (held items
// included), after STAB and type effectiveness, before the 85-100 % roll (mirrors Cmd_damagecalc +
// Cmd_typecalc).
static int ExpectedBase(struct BattleSim *sim, int atk, int def, u16 move)
{
    s32 d;
    int i;
    u8 type = gBattleMoves[move].type;

    Sim_Bind(sim);
    sim->critMultiplier = 1;
    d = CalculateBaseDamage(&B(atk), &B(def), move, 0, 0, 0, atk, def);
    if (B(atk).type1 == type || B(atk).type2 == type)
        d = d * 15 / 10;
    for (i = 0; gTypeEffectiveness[i] != TE_ENDTABLE; i += 3)
    {
        if (gTypeEffectiveness[i] == TE_FORESIGHT)
        {
            if (STATUS2(def) & STATUS2_FORESIGHT)
                break;
            continue;
        }
        if (gTypeEffectiveness[i] != type)
            continue;
        if (gTypeEffectiveness[i + 1] == B(def).type1)
            d = d * gTypeEffectiveness[i + 2] / 10;
        if (gTypeEffectiveness[i + 1] == B(def).type2 && B(def).type1 != B(def).type2)
            d = d * gTypeEffectiveness[i + 2] / 10;
    }
    return d;
}

static int ExpectedBaseItems(struct BattleSim *sim, int atk, int def, u16 move, u16 atkItem, u16 defItem)
{
    u16 sa = B(atk).item, sd = B(def).item;
    int d;
    B(atk).item = atkItem;
    B(def).item = defItem;
    d = ExpectedBase(sim, atk, def, move);
    B(atk).item = sa;
    B(def).item = sd;
    return d;
}

static int RollMatches(int base, int actual)
{
    int r;
    for (r = 0; r < 16; r++)
    {
        int d = base * (100 - r) / 100;
        if (d == 0) d = 1;
        if (d == actual) return 1;
    }
    return 0;
}

static int Dealt(struct BattleSim *sim) { return MAXHP(1) - HP(1); }

// First log entry of `id` in `turn` (0-based, or SC_ANY_TURN) whichever battler printed it. Item messages
// shown in the middle of a move (ITEMEFFECT_MOVE_END cures) go through printstring, which uses
// gBattlerAttacker, so the log's battler is the mover, not the holder; LOG_INDEX_T(id, holder, ..)
// would return -1 for them.
static int LogAt(struct BattleSim *sim, u16 id, int turn)
{
    int i;
    for (i = 0; i < sim->logCount; i++)
        if (sim->log[i].stringId == id && (turn == SC_ANY_TURN || sim->log[i].turn == turn))
            return i;
    return -1;
}

// one clean hit on turn 0: target alive, damaged, no crit, no status that deals end-of-turn damage
static int CleanHit(struct BattleSim *sim)
{
    return HP(1) > 0 && HP(1) < MAXHP(1) && !LOG_HAS(STRINGID_CRITICALHIT) && STATUS1(1) == 0
        && FirstMoveOf(sim, 0) != MOVE_NONE;
}
static int WantAtkBoost(struct BattleSim *sim)
{
    return CleanHit(sim) && Dealt(sim) > ExpectedBaseItems(sim, 0, 1, FirstMoveOf(sim, 0), ITEM_NONE, B(1).item);
}
static int WantDefBoost(struct BattleSim *sim)
{
    return CleanHit(sim) && Dealt(sim) < ExpectedBaseItems(sim, 0, 1, FirstMoveOf(sim, 0), B(0).item, ITEM_NONE) * 85 / 100;
}
// The attacker's item raises the damage: the actual damage is one of the 16 rolls of the boosted
// base and exceeds the best roll without the item.
static void CheckAtkBoost(struct BattleSim *sim)
{
    u16 move = FirstMoveOf(sim, 0);
    int base = ExpectedBase(sim, 0, 1, move), plain = ExpectedBaseItems(sim, 0, 1, move, ITEM_NONE, B(1).item);
    CHECK(CleanHit(sim), "clean hit (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(base > plain, "item raises the base damage (%d vs %d)", base, plain);
    CHECK(RollMatches(base, Dealt(sim)), "dealt %d is a roll of %d (unboosted %d)", Dealt(sim), base, plain);
    CHECK(Dealt(sim) > plain, "dealt %d beyond the unboosted maximum %d", Dealt(sim), plain);
}
// The defender's item lowers the damage below anything possible without it.
static void CheckDefBoost(struct BattleSim *sim)
{
    u16 move = FirstMoveOf(sim, 0);
    int base = ExpectedBase(sim, 0, 1, move), plain = ExpectedBaseItems(sim, 0, 1, move, B(0).item, ITEM_NONE);
    CHECK(CleanHit(sim), "clean hit (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(base < plain, "item lowers the base damage (%d vs %d)", base, plain);
    CHECK(RollMatches(base, Dealt(sim)), "dealt %d is a roll of %d (unboosted %d)", Dealt(sim), base, plain);
    CHECK(Dealt(sim) < plain * 85 / 100, "dealt %d below the unboosted minimum %d", Dealt(sim), plain * 85 / 100);
}
// Neither item changes the damage.
static void CheckNoBoost(struct BattleSim *sim)
{
    u16 move = FirstMoveOf(sim, 0);
    int base = ExpectedBase(sim, 0, 1, move), plain = ExpectedBaseItems(sim, 0, 1, move, ITEM_NONE, ITEM_NONE);
    CHECK(CleanHit(sim), "clean hit (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(base == plain, "items do not change the base damage (%d vs %d)", base, plain);
    CHECK(RollMatches(plain, Dealt(sim)), "dealt %d is a roll of %d", Dealt(sim), plain);
}

// ---------------------------------------------------------------------------------------------
// HP berries
static void CheckOranHalf(struct BattleSim *sim)
{
    CHECK(MAXHP(0) == 235, "snorlax max hp 235 (got %d)", MAXHP(0));
    CHECK(HP(0) == 127, "oran +10 at exactly half hp (hp %d)", HP(0));
    CHECK(B(0).item == ITEM_NONE, "berry consumed");
    CHECK(PARTY_ITEM(B_SIDE_PLAYER, 0) == ITEM_NONE, "party mon lost the berry too");
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMRESTOREDHEALTH, 0), "restored health message");
}
static void CheckOranAboveHalf(struct BattleSim *sim)
{
    CHECK(HP(0) == 118, "no heal above half (hp %d)", HP(0));
    CHECK(B(0).item == ITEM_ORAN_BERRY, "berry kept");
    CHECK(!LOG_HAS(STRINGID_PKMNSITEMRESTOREDHEALTH), "no message");
}
static void CheckSitrus30(struct BattleSim *sim)
{
    CHECK(HP(0) == 130, "sitrus +30 (hp %d)", HP(0));
    CHECK(B(0).item == ITEM_NONE, "consumed");
}
static void CheckSitrusCapped(struct BattleSim *sim)
{
    CHECK(MAXHP(0) < 49, "small max hp (%d)", MAXHP(0));
    CHECK(HP(0) == MAXHP(0), "healed to full, not beyond (hp %d/%d)", HP(0), MAXHP(0));
    CHECK(B(0).item == ITEM_NONE, "consumed");
}
static void CheckBerryJuice(struct BattleSim *sim)
{
    CHECK(HP(0) == 137, "berry juice +20 (hp %d)", HP(0));
    CHECK(B(0).item == ITEM_NONE, "berry juice consumed");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMRESTOREDHEALTH), "restored health message");
}
static void CheckBerryEndOfTurnOnly(struct BattleSim *sim)
{
    // Hitmonlee's Seismic Toss (50) drops Snorlax to 80 (<= 117); Snorlax then moves, and only at
    // the end of the turn the Oran Berry heals it.
    CHECK(MOVED_BEFORE(1, 0, 0), "hitmonlee hit first");
    CHECK(LOG_INDEX_T(STRINGID_PKMNSITEMRESTOREDHEALTH, 0, 0) > LOG_INDEX_T(STRINGID_USEDMOVE, 0, 0), "berry eaten after the holder's move");
    CHECK(HP(0) == 90, "80 + 10 (hp %d)", HP(0));
}
static void CheckBerryHolderFaints(struct BattleSim *sim)
{
    CHECK(HP(0) == 0, "holder fainted");
    CHECK(!LOG_HAS(STRINGID_PKMNSITEMRESTOREDHEALTH), "no berry for a fainted mon");
    CHECK(PARTY_ITEM(B_SIDE_PLAYER, 0) == ITEM_ORAN_BERRY, "berry kept");
}
static void CheckEnemySitrus(struct BattleSim *sim)
{
    CHECK(HP(1) == 130, "enemy sitrus +30 (hp %d)", HP(1));
    CHECK(B(1).item == ITEM_NONE && PARTY_ITEM(B_SIDE_OPPONENT, 0) == ITEM_NONE, "enemy berry consumed");
}
static void CheckFigyNeutral(struct BattleSim *sim)
{
    CHECK(HP(0) == 117 + 29, "figy heals 1/8 = 29 (hp %d)", HP(0));
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "hardy nature: no confusion");
    CHECK(!LOG_HAS(STRINGID_FORXCOMMAYZ), "no 'too spicy' message");
    CHECK(B(0).item == ITEM_NONE, "consumed");
}
static void CheckFigyDisliked(struct BattleSim *sim)
{
    CHECK(HP(0) == 117 + 29, "healed 1/8 (hp %d)", HP(0));
    CHECK(STATUS2(0) & STATUS2_CONFUSION, "disliked flavor confuses");
    CHECK(LOG_HAS(STRINGID_FORXCOMMAYZ), "'too spicy' message");
    CHECK(B(0).item == ITEM_NONE, "consumed");
}
static void CheckFlavorLiked(struct BattleSim *sim)
{
    CHECK(HP(0) == 117 + 29, "healed 1/8 (hp %d)", HP(0));
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "liked flavor: no confusion");
    CHECK(B(0).item == ITEM_NONE, "consumed");
}
static void CheckFigyOwnTempo(struct BattleSim *sim)
{
    CHECK(HP(0) == 82 + 20, "slowpoke healed 1/8 = 20 (hp %d)", HP(0));
    CHECK(LOG_HAS(STRINGID_FORXCOMMAYZ), "message still printed");
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "own tempo blocks the confusion");
    CHECK(B(0).item == ITEM_NONE, "consumed");
}

// ---------------------------------------------------------------------------------------------
// stat berries
static void CheckLiechi(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 7, "attack +1 (stage %d)", STAGE(0, STAT_ATK));
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(LOG_HAS(STRINGID_USINGITEMSTATOFPKMNROSE), "'using item, stat rose' message");
}
static void CheckLiechiAboveQuarter(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 6, "no boost above 1/4 (stage %d)", STAGE(0, STAT_ATK));
    CHECK(B(0).item == ITEM_LIECHI_BERRY, "kept");
}
static void CheckGanlon(struct BattleSim *sim) { CHECK(STAGE(0, STAT_DEF) == 7 && B(0).item == ITEM_NONE, "defense +1 (stage %d)", STAGE(0, STAT_DEF)); }
static void CheckPetaya(struct BattleSim *sim) { CHECK(STAGE(0, STAT_SPATK) == 7 && B(0).item == ITEM_NONE, "sp.atk +1 (stage %d)", STAGE(0, STAT_SPATK)); }
static void CheckApicot(struct BattleSim *sim) { CHECK(STAGE(0, STAT_SPDEF) == 7 && B(0).item == ITEM_NONE, "sp.def +1 (stage %d)", STAGE(0, STAT_SPDEF)); }
static void CheckSalac(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_SPEED) == 7 && B(0).item == ITEM_NONE, "speed +1 (stage %d)", STAGE(0, STAT_SPEED));
    CHECK(MOVED_BEFORE(1, 0, 0), "growlithe faster on turn 1");
    CHECK(MOVED_BEFORE(0, 1, 1), "snorlax faster on turn 2 after salac");
}
static void CheckLiechiAtMax(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 12 && HP(0) == 1, "belly drum maxed attack at 1 hp (stage %d hp %d)", STAGE(0, STAT_ATK), HP(0));
    CHECK(B(0).item == ITEM_LIECHI_BERRY, "liechi not eaten when attack is already maxed");
    CHECK(!LOG_HAS(STRINGID_USINGITEMSTATOFPKMNROSE), "no berry message");
}
static void CheckLansat(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_FOCUS_ENERGY, "focus energy status");
    CHECK(LOG_HAS(STRINGID_PKMNUSEDXTOGETPUMPED), "'got pumped' message");
    CHECK(B(0).item == ITEM_NONE, "consumed");
}
static void CheckLansatAlreadyPumped(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_FOCUS_ENERGY, "focus energy from the move");
    CHECK(B(0).item == ITEM_LANSAT_BERRY, "lansat kept");
    CHECK(!LOG_HAS(STRINGID_PKMNUSEDXTOGETPUMPED), "no berry message");
}
static int WantStarfSpeed(struct BattleSim *sim) { return STAGE(0, STAT_SPEED) == 8; }
static void CheckStarf(struct BattleSim *sim)
{
    int s, raised = 0;
    for (s = STAT_ATK; s <= STAT_SPDEF; s++)
        if (STAGE(0, s) == 8) raised++;
        else CHECK(STAGE(0, s) == 6, "other stat %d untouched (stage %d)", s, STAGE(0, s));
    CHECK(raised == 1, "exactly one stat +2 (got %d)", raised);
    CHECK(STAGE(0, STAT_SPEED) == 8, "this seed: speed");
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(LOG_HAS(STRINGID_USINGITEMSTATOFPKMNROSE), "stat rose message");
}
static void CheckLeppa(struct BattleSim *sim)
{
    CHECK(PP(0, 0) == 5, "extreme speed restored to its 5 pp max (pp %d)", PP(0, 0));
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMRESTOREDPP), "restored pp message");
}
static void CheckLeppaFirstZeroSlot(struct BattleSim *sim)
{
    // slot 0 (Tackle) was at 0 PP from the start: it gets the 10 PP, not the move just used
    CHECK(PP(0, 0) == 10, "tackle +10 (pp %d)", PP(0, 0));
    CHECK(PP(0, 1) == 4, "extreme speed untouched (pp %d)", PP(0, 1));
    CHECK(B(0).item == ITEM_NONE, "consumed");
}
static void CheckLeppaNotNeeded(struct BattleSim *sim)
{
    CHECK(PP(0, 0) == 4 && B(0).item == ITEM_LEPPA_BERRY, "no 0-pp move: leppa kept (pp %d)", PP(0, 0));
}

// ---------------------------------------------------------------------------------------------
// status berries / herbs
static void CheckCheriAfterMove(struct BattleSim *sim)
{
    int cure = LogAt(sim, STRINGID_PKMNSITEMCUREDPARALYSIS, 0);
    CHECK(MOVED_BEFORE(1, 0, 0), "jolteon first");
    CHECK(STATUS1(0) == 0, "paralysis cured");
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(cure >= 0, "cured message on turn 1");
    CHECK(cure > LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0) && cure < LOG_INDEX_T(STRINGID_USEDMOVE, 0, 0), "cured right after thunder wave, before the holder moves (idx %d)", cure);
}
static void CheckCheriInitialStatus(struct BattleSim *sim)
{
    // Paralyzed from the start: nothing cures it at battle start (ITEMEFFECT_ON_SWITCH_IN only handles
    // Amulet Coin / White Herb); the first moveend (Rattata's Splash) does.
    int cure = LogAt(sim, STRINGID_PKMNSITEMCUREDPARALYSIS, 0);
    CHECK(STATUS1(0) == 0 && B(0).item == ITEM_NONE, "cured, consumed");
    CHECK(cure >= 0 && cure > LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0), "after the foe's move (idx %d)", cure);
    CHECK(cure >= 0 && cure < LOG_INDEX_T(STRINGID_USEDMOVE, 0, 0), "before the holder's own move (idx %d)", cure);
}
static void CheckChestoRest(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0), "rest healed");
    CHECK(STATUS1(0) == 0, "chesto woke it up immediately");
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMWOKEIT), "woke up message");
}
static void CheckChestoYawn(struct BattleSim *sim)
{
    // Yawn puts the holder to sleep at the end of turn 2 (ENDTURN_YAWN); ENDTURN_ITEMS2 cures it right away.
    CHECK(LOG_HAS_T(STRINGID_PKMNFELLASLEEP, 1), "fell asleep at the end of turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMWOKEIT, 1), "woke in the same end phase");
    CHECK(STATUS1(0) == 0 && B(0).item == ITEM_NONE, "awake, berry gone");
}
static int WantCuredPoison(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNSITEMCUREDPOISON, SC_ANY_TURN); }
static void CheckPecha(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0, "toxic cured incl. counter (status %x)", STATUS1(0));
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(HP(0) == MAXHP(0), "no poison damage taken");
}
static int WantHealedBurn(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNSITEMHEALEDBURN, SC_ANY_TURN); }
static void CheckRawst(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0 && B(0).item == ITEM_NONE, "burn cured, consumed");
    CHECK(HP(0) == MAXHP(0), "no burn damage");
}
static int WantDefrosted(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNSITEMDEFROSTEDIT, SC_ANY_TURN); }
static void CheckAspear(struct BattleSim *sim) { CHECK(STATUS1(0) == 0 && B(0).item == ITEM_NONE, "freeze cured, consumed"); }
static void CheckPersim(struct BattleSim *sim)
{
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION), "confusion cured");
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMSNAPPEDOUT), "snapped out message");
}
static void CheckLumParalysis(struct BattleSim *sim)
{
    CHECK(STATUS1(0) == 0 && B(0).item == ITEM_NONE, "cured, consumed");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMCUREDPROBLEM), "cured problem message");
}
static void CheckLumConfusion(struct BattleSim *sim)
{
    CHECK(!(STATUS2(0) & STATUS2_CONFUSION) && B(0).item == ITEM_NONE, "confusion cured, consumed");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMCUREDPROBLEM), "cured problem message");
}
static void CheckMentalHerb(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNFELLINLOVE), "attract worked");
    CHECK(!(STATUS2(0) & STATUS2_INFATUATION), "infatuation cured");
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMCUREDPROBLEM), "cured problem message");
}
static void CheckMentalHerbConfusion(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_CONFUSION, "mental herb does not cure confusion");
    CHECK(B(0).item == ITEM_MENTAL_HERB, "kept");
}
static void CheckWhiteHerbGrowl(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 6, "attack restored (stage %d)", STAGE(0, STAT_ATK));
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMRESTOREDSTATUS, 0), "restored status message");
}
static void CheckWhiteHerbIntimidate(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 6, "intimidate undone (stage %d)", STAGE(0, STAT_ATK));
    CHECK(B(0).item == ITEM_NONE, "consumed");
    CHECK(LOG_INDEX_T(STRINGID_PKMNSITEMRESTOREDSTATUS, 0, 0) >= 0
       && LOG_INDEX_T(STRINGID_PKMNSITEMRESTOREDSTATUS, 0, 0) < LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0), "used at switch-in, before any move");
}
static void CheckWhiteHerbKeepsPositive(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_ATK) == 7, "+2 -1 = +1 is not negative (stage %d)", STAGE(0, STAT_ATK));
    CHECK(B(0).item == ITEM_WHITE_HERB, "herb kept");
}
static int WantOverheatHit(struct BattleSim *sim) { return HP(1) < MAXHP(1); }
static void CheckWhiteHerbOverheat(struct BattleSim *sim)
{
    CHECK(STAGE(0, STAT_SPATK) == 6, "self-inflicted -2 restored (stage %d)", STAGE(0, STAT_SPATK));
    CHECK(B(0).item == ITEM_NONE, "consumed");
}

// ---------------------------------------------------------------------------------------------
// Leftovers
static void CheckLeftovers(struct BattleSim *sim)
{
    CHECK(HP(0) == 214, "+1/16 = 14 (hp %d)", HP(0));
    CHECK(B(0).item == ITEM_LEFTOVERS, "not consumed");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE), "restored a little message");
}
static void CheckLeftoversFull(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0) && !LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE), "nothing at full hp");
}
static void CheckLeftoversMin1(struct BattleSim *sim)
{
    CHECK(MAXHP(0) < 16, "max hp below 16 (%d)", MAXHP(0));
    CHECK(HP(0) == 11, "heals at least 1 (hp %d)", HP(0));
}
static void CheckLeftoversCap(struct BattleSim *sim) { CHECK(HP(0) == MAXHP(0), "capped at max (hp %d/%d)", HP(0), MAXHP(0)); }
static void CheckIngrainBeforeLeftovers(struct BattleSim *sim)
{
    CHECK(HP(0) == MAXHP(0), "full");
    CHECK(LOG_HAS(STRINGID_PKMNABSORBEDNUTRIENTS), "ingrain healed the last point");
    CHECK(!LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE), "leftovers found the holder already at full hp");
}
static void CheckIngrainThenLeftovers(struct BattleSim *sim)
{
    CHECK(HP(0) == 100 + 7 + 7, "ingrain 1/16 then leftovers 1/16 (hp %d)", HP(0));
    CHECK(LOG_INDEX_T(STRINGID_PKMNABSORBEDNUTRIENTS, 0, 0) < LOG_INDEX_T(STRINGID_PKMNSITEMRESTOREDHPALITTLE, 0, 0), "ingrain first");
}
static int WantSeeded(struct BattleSim *sim) { return (STATUS3(0) & STATUS3_LEECHSEED) != 0; }
static void CheckLeftoversBeforeLeechSeed(struct BattleSim *sim)
{
    CHECK(HP(0) == 235 - 29, "leftovers to full, then leech seed 1/8 (hp %d)", HP(0));
    CHECK(LOG_INDEX_T(STRINGID_PKMNSITEMRESTOREDHPALITTLE, 0, 0) < LOG_INDEX_T(STRINGID_PKMNSAPPEDBYLEECHSEED, 0, 0), "leftovers before leech seed");
}
static void CheckLeftoversBeforePoison(struct BattleSim *sim)
{
    CHECK(HP(0) == 300 + 20 - 40, "chansey +1/16 then -1/8 poison (hp %d)", HP(0));
}

// ---------------------------------------------------------------------------------------------
// Choice Band
static void CheckChoiceLock(struct BattleSim *sim)
{
    CHECK(CHOICED(0) == MOVE_TACKLE, "locked into tackle (%d)", CHOICED(0));
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 1), "forced tackle on turn 2");
    CHECK(!LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "growl not selectable");
}
static void CheckChoicePpOut(struct BattleSim *sim)
{
    CHECK(PP(0, 0) == 0, "tackle out of pp");
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 1), "struggle on turn 2");
}
// Disable is only 55 % accurate: require that it landed on turn 1 (Rattata's last move was Tackle).
static int WantTackleDisabled(struct BattleSim *sim)
{
    return Sc_LogHas(sim, STRINGID_PKMNMOVEWASDISABLED, 0) && sim->disableStructs[0].disabledMove == MOVE_TACKLE;
}
static void CheckChoiceDisabled(struct BattleSim *sim)
{
    // CheckMoveLimitations: Tackle is disabled, the other slots are not the choiced move -> all four
    // unusable -> Struggle.
    CHECK(LOG_HAS_T(STRINGID_PKMNMOVEWASDISABLED, 0) && sim->disableStructs[0].disabledMove == MOVE_TACKLE, "tackle disabled on turn 1");
    CHECK(CHOICED(0) == MOVE_TACKLE, "still locked into tackle (%d)", CHOICED(0));
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 1), "choice-locked + disabled: struggle");
    CHECK(!LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 1) && !LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "neither tackle nor growl on turn 2");
}
static void CheckChoiceStruggleNoLock(struct BattleSim *sim)
{
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_STRUGGLE, 0), "struggled");
    CHECK(CHOICED(0) == MOVE_NONE, "struggle does not set the lock (%d)", CHOICED(0));
}
static void CheckChoiceKnockOffUnlocks(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_NONE, "band knocked off");
    CHECK(CHOICED(0) == MOVE_NONE, "lock cleared (%d)", CHOICED(0));
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 2), "growl usable on turn 3");
}
static void CheckChoiceTrick(struct BattleSim *sim)
{
    CHECK(B(1).item == ITEM_CHOICE_BAND && B(0).item == ITEM_NONE, "band tricked onto snorlax");
    CHECK(CHOICED(0) == MOVE_NONE, "trick user not locked (%d)", CHOICED(0));
    CHECK(CHOICED(1) == MOVE_TACKLE, "snorlax locked by its first move with the band (%d)", CHOICED(1));
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_TACKLE, 1) && !LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 1), "snorlax forced into tackle on turn 2");
}
static void CheckChoiceSwitchReset(struct BattleSim *sim)
{
    CHECK(CHOICED(0) == MOVE_GROWL, "new lock after switching back (%d)", CHOICED(0));
    CHECK(LOG_HAS_MOVE_T(STRINGID_USEDMOVE, MOVE_GROWL, 3), "growl usable after the switch reset");
}

// ---------------------------------------------------------------------------------------------
// speed items
static int WantQuickClawOnce(struct BattleSim *sim) { return MOVED_BEFORE(0, 1, 0) && MOVED_BEFORE(1, 0, 1); }
static void CheckQuickClaw(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(0, 1, 0), "slow snorlax first on turn 1 (quick claw)");
    CHECK(MOVED_BEFORE(1, 0, 1), "normal order on turn 2");
    CHECK(B(0).item == ITEM_QUICK_CLAW, "not consumed");
}
static void CheckQuickClawPriority(struct BattleSim *sim)
{
    int t;
    for (t = 0; t < 5; t++)
        CHECK(MOVED_BEFORE(1, 0, t), "quick attack still first on turn %d", t + 1);
}
static void CheckMachoBrace(struct BattleSim *sim) { CHECK(MOVED_BEFORE(1, 0, 0), "rattata's speed halved: machop first"); }

// ---------------------------------------------------------------------------------------------
// King's Rock / Focus Band / Shell Bell / accuracy
static int WantFlinch(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_PKMNFLINCHED, 0); }
static void CheckKingsRock(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNFLINCHED, 0), "flinched");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 1, 0) < 0, "target lost its move");
    CHECK(B(0).item == ITEM_KINGS_ROCK, "not consumed");
}
static void CheckKingsRockUnflagged(struct BattleSim *sim)
{
    // Hyper Voice is a plain EFFECT_HIT move that lacks FLAG_KINGS_ROCK_AFFECTED, so the 10 % roll in
    // ITEMEFFECT_KINGSROCK_SHELLBELL can never turn into a flinch (whatever the seed).
    CHECK(!LOG_HAS(STRINGID_PKMNFLINCHED), "hyper voice is not king's rock affected");
    CHECK(HP(1) > 0 && HP(1) < MAXHP(1), "steelix was hit and survived (hp %d/%d)", HP(1), MAXHP(1));
    CHECK(LOG_COUNT(STRINGID_USEDMOVE) == 10, "both battlers moved every turn (%d moves)", LOG_COUNT(STRINGID_USEDMOVE));
}
static int WantHungOn(struct BattleSim *sim) { return HP(0) == 1; }
static int WantNotHungOn(struct BattleSim *sim) { return HP(0) == 0; }
static void CheckFocusBandHolds(struct BattleSim *sim)
{
    CHECK(HP(0) == 1, "hung on at 1 hp");
    CHECK(LOG_HAS(STRINGID_PKMNHUNGONWITHX), "hung on message");
    CHECK(B(0).item == ITEM_FOCUS_BAND, "not consumed");
}
static void CheckFocusBandFails(struct BattleSim *sim)
{
    CHECK(HP(0) == 0 && !LOG_HAS(STRINGID_PKMNHUNGONWITHX), "no activation this seed");
}
static void CheckShellBell(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1) - 50, "seismic toss 50");
    CHECK(HP(0) == 85 + 6, "healed 50/8 = 6 (hp %d)", HP(0));
    CHECK(LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE), "restored a little message");
    CHECK(B(0).item == ITEM_SHELL_BELL, "not consumed");
}
static void CheckShellBellFull(struct BattleSim *sim)
{
    CHECK(HP(1) == MAXHP(1) - 50 && HP(0) == MAXHP(0), "no heal at full hp");
    CHECK(!LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE), "no message");
}
static void CheckShellBellMinimum(struct BattleSim *sim)
{
    // dmg / 8 == 0 is clamped to a 1 HP heal (ITEMEFFECT_KINGSROCK_SHELLBELL): a resisted Tackle on
    // Steelix's 200 Defense deals well under 8.
    int dealt = MAXHP(1) - HP(1);
    CHECK(dealt > 0 && dealt < 8, "weak hit (dealt %d)", dealt);
    CHECK(HP(0) == 50 + 1, "healed the 1 hp minimum (hp %d)", HP(0));
    CHECK(LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE), "restored a little message");
}
static void CheckShellBellSubstitute(struct BattleSim *sim)
{
    // Rattata (105 hp) makes a 26 hp substitute; Seismic Toss (50) breaks it. Shell Bell heals from
    // gSpecialStatuses[target].dmg, which datahpupdate sets to the substitute HP actually lost (26),
    // not gBattleMoveDamage: 26 / 8 = 3.
    CHECK(MOVED_BEFORE(1, 0, 0), "rattata's substitute first (snorlax is slower)");
    CHECK(STATUS2(1) & STATUS2_SUBSTITUTE ? 0 : 1, "substitute broken");
    CHECK(HP(1) == 105 - 26, "only the substitute cost (hp %d)", HP(1));
    CHECK(HP(0) == 85 + 3, "healed 26/8 = 3 from hitting the substitute (hp %d)", HP(0));
    CHECK(LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE), "restored a little message");
}
static int WantMiss(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_ATTACKMISSED, 0); }
static void CheckEvasionItemMiss(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_ATTACKMISSED) && HP(1) == MAXHP(1), "a 100 %% accurate move missed");
}

// ---------------------------------------------------------------------------------------------
// crit items (only reachability: the crit stage itself is not observable in one run)
static int WantCrit(struct BattleSim *sim) { return Sc_LogHas(sim, STRINGID_CRITICALHIT, 0); }
static void CheckCrit(struct BattleSim *sim) { CHECK(LOG_HAS_T(STRINGID_CRITICALHIT, 0) && HP(1) < MAXHP(1), "critical hit landed"); }

// ---------------------------------------------------------------------------------------------
// Knock Off / Thief / Trick / Recycle
static void CheckKnockOff(struct BattleSim *sim)
{
    CHECK(B(1).item == ITEM_NONE, "leftovers knocked off");
    CHECK(PARTY_ITEM(B_SIDE_OPPONENT, 0) == ITEM_LEFTOVERS, "party data keeps the item (only the battle copy loses it)");
    CHECK(LOG_HAS(STRINGID_PKMNKNOCKEDOFF), "knocked off message");
    CHECK(sim->wishFutureKnock.knockedOffMons[B_SIDE_OPPONENT] & 1, "knocked-off flag set");
    CHECK(HP(1) < 200 && !LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE), "no leftovers heal afterwards (hp %d)", HP(1));
}
static void CheckKnockOffStickyHold(struct BattleSim *sim)
{
    CHECK(B(1).item == ITEM_LEFTOVERS, "sticky hold keeps the item");
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEYINEFFECTIVE), "sticky hold message");
    CHECK(HP(1) < MAXHP(1), "damage still dealt");
}
static void CheckKnockOffNoItem(struct BattleSim *sim)
{
    CHECK(!LOG_HAS(STRINGID_PKMNKNOCKEDOFF) && HP(1) < MAXHP(1), "nothing to knock off, damage dealt");
    CHECK(!(sim->wishFutureKnock.knockedOffMons[B_SIDE_OPPONENT] & 1), "flag not set");
}
static void CheckKnockOffPreventsBerry(struct BattleSim *sim)
{
    CHECK(B(1).item == ITEM_NONE, "oran knocked off");
    CHECK(HP(1) < 117 && !LOG_HAS(STRINGID_PKMNSITEMRESTOREDHEALTH), "berry never activates (hp %d)", HP(1));
}
static void CheckThief(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_NONE, "stolen");
    CHECK(PARTY_ITEM(B_SIDE_PLAYER, 0) == ITEM_LEFTOVERS && PARTY_ITEM(B_SIDE_OPPONENT, 0) == ITEM_NONE, "party data updated on both sides");
    CHECK(LOG_HAS(STRINGID_PKMNSTOLEITEM), "stole message");
}
static void CheckThiefHoldingItem(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_ORAN_BERRY && B(1).item == ITEM_LEFTOVERS, "no steal while holding an item");
    CHECK(!LOG_HAS(STRINGID_PKMNSTOLEITEM), "no message");
}
static void CheckThiefByOpponent(struct BattleSim *sim)
{
    // SetMoveEffect MOVE_EFFECT_STEAL_ITEM: an attacker on B_SIDE_OPPONENT skips the steal outside
    // link / Battle Tower / e-Reader battles. Snorlax starts at 200 so the Leftovers heal (235/16 = 14)
    // it still gets at the end of the turn does not hide Thief's damage.
    int dealt = 200 + 14 - HP(0);
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_NONE, "trainer's mon cannot steal from the player");
    CHECK(!LOG_HAS(STRINGID_PKMNSTOLEITEM), "no message");
    CHECK(LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE), "leftovers kept working for the player");
    CHECK(!LOG_HAS(STRINGID_CRITICALHIT) && dealt > 0 && RollMatches(ExpectedBase(sim, 1, 0, MOVE_THIEF), dealt), "thief still hit for a normal roll (dealt %d)", dealt);
}
static void CheckThiefStickyHold(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_NONE && B(1).item == ITEM_LEFTOVERS, "sticky hold");
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEYINEFFECTIVE), "sticky hold message");
}
static void CheckThiefSubstitute(struct BattleSim *sim)
{
    CHECK(MOVED_BEFORE(1, 0, 0), "substitute up first");
    CHECK(B(0).item == ITEM_NONE && B(1).item == ITEM_LEFTOVERS, "substitute blocks the steal");
}
static void CheckThiefAfterKnockedOff(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_NONE, "rattata was knocked off");
    CHECK(B(1).item == ITEM_LEFTOVERS && !LOG_HAS(STRINGID_PKMNSTOLEITEM), "a knocked-off mon cannot steal");
}
static void CheckTrick(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_TWISTED_SPOON, "swapped");
    CHECK(PARTY_ITEM(B_SIDE_PLAYER, 0) == ITEM_LEFTOVERS && PARTY_ITEM(B_SIDE_OPPONENT, 0) == ITEM_TWISTED_SPOON, "party data swapped");
    CHECK(LOG_HAS(STRINGID_PKMNSWITCHEDITEMS), "switched items message");
}
static void CheckTrickGives(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_NONE && B(1).item == ITEM_TWISTED_SPOON, "gave the item away");
    CHECK(LOG_HAS(STRINGID_PKMNSWITCHEDITEMS), "message");
}
static void CheckTrickFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "trick failed");
    CHECK(!LOG_HAS(STRINGID_PKMNSWITCHEDITEMS), "no swap");
}
static void CheckTrickByOpponent(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED), "opponent's trick fails");
    CHECK(B(0).item == ITEM_LEFTOVERS && B(1).item == ITEM_TWISTED_SPOON, "items unchanged");
}
static void CheckTrickStickyHold(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_PKMNSXMADEYINEFFECTIVE), "sticky hold message");
    CHECK(B(0).item == ITEM_TWISTED_SPOON && B(1).item == ITEM_LEFTOVERS, "items unchanged");
}
static void CheckTrickAfterKnockOff(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNKNOCKEDOFF, 0), "knocked off on turn 1");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1), "trick fails on a knocked-off target");
    CHECK(B(0).item == ITEM_TWISTED_SPOON && B(1).item == ITEM_NONE, "items unchanged");
}
static void CheckRecycle(struct BattleSim *sim)
{
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMRESTOREDHEALTH, 0), "oran eaten on turn 1");
    CHECK(LOG_HAS_T(STRINGID_XFOUNDONEY, 1), "recycled on turn 2");
    CHECK(LOG_HAS_T(STRINGID_PKMNSITEMRESTOREDHEALTH, 1), "eaten again at the end of turn 2");
    CHECK(HP(0) == 47 + 10 + 10, "healed twice (hp %d)", HP(0));
    CHECK(B(0).item == ITEM_NONE, "eaten again");
}
static void CheckRecycleFails(struct BattleSim *sim)
{
    CHECK(LOG_HAS(STRINGID_BUTITFAILED) && !LOG_HAS(STRINGID_XFOUNDONEY), "recycle failed");
}
static void CheckRecycleAfterKnockOff(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_NONE, "knocked off");
    CHECK(LOG_HAS_T(STRINGID_BUTITFAILED, 1) && !LOG_HAS(STRINGID_XFOUNDONEY), "a knocked-off item is not 'used': recycle fails");
}
static void CheckRecycleHolding(struct BattleSim *sim)
{
    CHECK(B(0).item == ITEM_LEFTOVERS && LOG_HAS(STRINGID_BUTITFAILED), "fails while holding an item");
}

// ---------------------------------------------------------------------------------------------
// misc
static void CheckAmuletCoin(struct BattleSim *sim) { CHECK(sim->sBattleStructStorage.moneyMultiplier == 2, "money doubled (x%d)", sim->sBattleStructStorage.moneyMultiplier); }
static void CheckSmokeBallTrapped(struct BattleSim *sim)
{
    CHECK(STATUS2(0) & STATUS2_ESCAPE_PREVENTION, "mean looked");
    CHECK(B(0).species == SPECIES_RATTATA, "smoke ball does not allow switching out of a trap");
    CHECK(LOG_INDEX_T(STRINGID_USEDMOVE, 0, 1) >= 0, "fell back to a move on turn 2");
}
static void CheckInert(struct BattleSim *sim)
{
    CheckNoBoost(sim);
    CHECK(!LOG_HAS(STRINGID_PKMNSITEMRESTOREDHEALTH) && !LOG_HAS(STRINGID_PKMNSITEMRESTOREDHPALITTLE)
       && !LOG_HAS(STRINGID_PKMNSITEMRESTOREDSTATUS), "no item messages");
    CHECK(B(0).item != ITEM_NONE && B(1).item != ITEM_NONE, "items untouched");
}

static const struct Scenario sScenarios[] =
{
    // ---- HP berries ----
    { .name = "oran_at_half_hp",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_ORAN_BERRY, 117) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOranHalf },
    { .name = "oran_above_half_no_trigger",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_ORAN_BERRY, 118) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckOranAboveHalf },
    { .name = "sitrus_heals_30",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_SITRUS_BERRY, 100) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSitrus30 },
    { .name = "sitrus_capped_at_max",
      .player = { MHP(SPECIES_RATTATA, 15, MOVE_SPLASH, ITEM_SITRUS_BERRY, 19) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckSitrusCapped },
    { .name = "berry_juice_heals_20",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_BERRY_JUICE, 117) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBerryJuice },
    { .name = "hp_berry_end_of_turn_only",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_ORAN_BERRY, 130) },
      .enemy = { M1(SPECIES_HITMONLEE, 50, MOVE_SEISMIC_TOSS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBerryEndOfTurnOnly },
    { .name = "hp_berry_holder_faints",
      .player = { MHP(SPECIES_RATTATA, 50, MOVE_SPLASH, ITEM_ORAN_BERRY, 20) },
      .enemy = { M1(SPECIES_HITMONLEE, 50, MOVE_SEISMIC_TOSS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckBerryHolderFaints },
    { .name = "enemy_sitrus",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .enemy = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_SITRUS_BERRY, 100) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckEnemySitrus },
    { .name = "figy_neutral_nature",
      .player = { MHPN(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_FIGY_BERRY, 117, NATURE_HARDY) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFigyNeutral },
    { .name = "figy_modest_confuses",
      .player = { MHPN(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_FIGY_BERRY, 117, NATURE_MODEST) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFigyDisliked },
    { .name = "wiki_adamant_confuses",
      .player = { MHPN(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_WIKI_BERRY, 117, NATURE_ADAMANT) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFigyDisliked },
    { .name = "aguav_naughty_confuses",
      .player = { MHPN(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_AGUAV_BERRY, 117, NATURE_NAUGHTY) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFigyDisliked },
    { .name = "iapapa_hasty_confuses",
      .player = { MHPN(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_IAPAPA_BERRY, 117, NATURE_HASTY) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFigyDisliked },
    { .name = "mago_timid_liked",
      .player = { MHPN(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_MAGO_BERRY, 117, NATURE_TIMID) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFlavorLiked },
    { .name = "figy_own_tempo_no_confusion",
      .player = { { .species = SPECIES_SLOWPOKE, .level = 50, .moves = { MOVE_SPLASH, SPLASH3 }, .item = ITEM_FIGY_BERRY, .hp = 82, .hpSet = 1, .nature = NATURE_MODEST, .abilityNum = 1 } },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckFigyOwnTempo },

    // ---- stat berries ----
    { .name = "liechi_at_quarter",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LIECHI_BERRY, 58) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLiechi },
    { .name = "liechi_above_quarter_no_trigger",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LIECHI_BERRY, 59) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLiechiAboveQuarter },
    { .name = "ganlon_defense",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_GANLON_BERRY, 58) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckGanlon },
    { .name = "salac_speed_changes_order",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_SALAC_BERRY, 58) },
      .enemy = { M1(SPECIES_GROWLITHE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckSalac },
    { .name = "petaya_sp_attack",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_PETAYA_BERRY, 58) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPetaya },
    { .name = "apicot_sp_defense",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_APICOT_BERRY, 58) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckApicot },
    { .name = "liechi_not_eaten_at_max_attack",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_BELLY_DRUM, ITEM_LIECHI_BERRY, 118) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLiechiAtMax },
    { .name = "lansat_focus_energy",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LANSAT_BERRY, 58) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLansat },
    { .name = "lansat_already_pumped",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_FOCUS_ENERGY, ITEM_LANSAT_BERRY, 58) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLansatAlreadyPumped },
    { .name = "starf_random_stat",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_STARF_BERRY, 58) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantStarfSpeed, .check = CheckStarf },
    { .name = "leppa_restores_pp",
      .player = { { .species = SPECIES_ARCANINE, .level = 50, .moves = { MOVE_EXTREME_SPEED, SPLASH3 }, .item = ITEM_LEPPA_BERRY, .pp = { 1, 40, 40, 40 }, .ppSet = 1 } },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLeppa },
    { .name = "leppa_first_empty_slot",
      .player = { { .species = SPECIES_ARCANINE, .level = 50, .moves = { MOVE_TACKLE, MOVE_EXTREME_SPEED, MOVE_SPLASH, MOVE_SPLASH }, .item = ITEM_LEPPA_BERRY, .pp = { 0, 5, 40, 40 }, .ppSet = 1 } },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(1, 0) }, .turns = 1, .check = CheckLeppaFirstZeroSlot },
    { .name = "leppa_no_empty_move",
      .player = { MI(SPECIES_ARCANINE, 50, MOVE_EXTREME_SPEED, ITEM_LEPPA_BERRY) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLeppaNotNeeded },

    // ---- status berries / herbs ----
    { .name = "cheri_cures_right_after_move",
      .player = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_CHERI_BERRY) },
      .enemy = { M1(SPECIES_JOLTEON, 50, MOVE_THUNDER_WAVE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCheriAfterMove },
    { .name = "cheri_initial_status_cured_after_first_move",
      .player = { { .species = SPECIES_SNORLAX, .level = 50, .moves = { MOVE_SPLASH, SPLASH3 }, .item = ITEM_CHERI_BERRY, .status = STATUS1_PARALYSIS } },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckCheriInitialStatus },
    { .name = "chesto_rest",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_REST, ITEM_CHESTO_BERRY, 100) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckChestoRest },
    { .name = "chesto_yawn_end_of_turn",
      .player = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_CHESTO_BERRY) },
      .enemy = { M1(SPECIES_SLOWPOKE, 50, MOVE_YAWN) },
      .actions = { T(0, 0), T(0, 1) }, .turns = 2, .check = CheckChestoYawn },
    { .name = "pecha_cures_toxic",
      .player = { MI(SPECIES_CHANSEY, 50, MOVE_SPLASH, ITEM_PECHA_BERRY) },
      .enemy = { M1(SPECIES_GRIMER, 50, MOVE_TOXIC) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCuredPoison, .check = CheckPecha },
    { .name = "rawst_cures_burn",
      .player = { MI(SPECIES_CHANSEY, 50, MOVE_SPLASH, ITEM_RAWST_BERRY) },
      .enemy = { M1(SPECIES_VULPIX, 50, MOVE_WILL_O_WISP) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHealedBurn, .check = CheckRawst },
    { .name = "aspear_cures_freeze",
      .player = { MI(SPECIES_CHANSEY, 50, MOVE_SPLASH, ITEM_ASPEAR_BERRY) },
      .enemy = { M1(SPECIES_DEWGONG, 50, MOVE_ICE_BEAM) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDefrosted, .check = CheckAspear },
    { .name = "persim_cures_confusion",
      .player = { MI(SPECIES_CHANSEY, 50, MOVE_SPLASH, ITEM_PERSIM_BERRY) },
      .enemy = { M1(SPECIES_HAUNTER, 50, MOVE_CONFUSE_RAY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckPersim },
    { .name = "lum_cures_paralysis",
      .player = { MI(SPECIES_CHANSEY, 50, MOVE_SPLASH, ITEM_LUM_BERRY) },
      .enemy = { M1(SPECIES_JOLTEON, 50, MOVE_THUNDER_WAVE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLumParalysis },
    { .name = "lum_cures_confusion",
      .player = { MI(SPECIES_CHANSEY, 50, MOVE_SPLASH, ITEM_LUM_BERRY) },
      .enemy = { M1(SPECIES_HAUNTER, 50, MOVE_CONFUSE_RAY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLumConfusion },
    { .name = "mental_herb_cures_attract",
      .player = { MI(SPECIES_NIDORAN_F, 50, MOVE_SPLASH, ITEM_MENTAL_HERB) },
      .enemy = { M1(SPECIES_NIDORAN_M, 50, MOVE_ATTRACT) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMentalHerb },
    { .name = "mental_herb_not_confusion",
      .player = { MI(SPECIES_CHANSEY, 50, MOVE_SPLASH, ITEM_MENTAL_HERB) },
      .enemy = { M1(SPECIES_HAUNTER, 50, MOVE_CONFUSE_RAY) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMentalHerbConfusion },
    { .name = "white_herb_after_growl",
      .player = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_WHITE_HERB) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_GROWL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWhiteHerbGrowl },
    { .name = "white_herb_intimidate_switch_in",
      .player = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_WHITE_HERB) },
      .enemy = { M1(SPECIES_GYARADOS, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWhiteHerbIntimidate },
    { .name = "white_herb_keeps_positive_stage",
      .player = { MI(SPECIES_SCYTHER, 50, MOVE_SWORDS_DANCE, ITEM_WHITE_HERB) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_GROWL) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckWhiteHerbKeepsPositive },
    { .name = "white_herb_self_drop_overheat",
      .player = { MI(SPECIES_ARCANINE, 50, MOVE_OVERHEAT, ITEM_WHITE_HERB) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantOverheatHit, .check = CheckWhiteHerbOverheat },

    // ---- Leftovers ----
    { .name = "leftovers_heals_16th",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS, 200) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLeftovers },
    { .name = "leftovers_full_hp_silent",
      .player = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLeftoversFull },
    { .name = "leftovers_minimum_1",
      .player = { MHP(SPECIES_RATTATA, 3, MOVE_SPLASH, ITEM_LEFTOVERS, 10) },
      .enemy = { M1(SPECIES_RATTATA, 3, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLeftoversMin1 },
    { .name = "leftovers_capped_at_max",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS, 234) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLeftoversCap },
    { .name = "ingrain_heals_before_leftovers",
      .player = { MHP(SPECIES_BULBASAUR, 50, MOVE_INGRAIN, ITEM_LEFTOVERS, 119) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIngrainBeforeLeftovers },
    { .name = "ingrain_then_leftovers",
      .player = { MHP(SPECIES_BULBASAUR, 50, MOVE_INGRAIN, ITEM_LEFTOVERS, 100) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckIngrainThenLeftovers },
    { .name = "leftovers_before_leech_seed",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS, 230) },
      .enemy = { M1(SPECIES_BULBASAUR, 50, MOVE_LEECH_SEED) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantSeeded, .check = CheckLeftoversBeforeLeechSeed },
    { .name = "leftovers_before_poison",
      .player = { { .species = SPECIES_CHANSEY, .level = 50, .moves = { MOVE_SPLASH, SPLASH3 }, .item = ITEM_LEFTOVERS, .hp = 300, .hpSet = 1, .status = STATUS1_POISON } },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckLeftoversBeforePoison },

    // ---- Choice Band ----
    { .name = "choice_band_physical_boost",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_STRENGTH, ITEM_CHOICE_BAND) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "choice_band_no_special_boost",
      .player = { MI(SPECIES_ALAKAZAM, 50, MOVE_PSYCHIC, ITEM_CHOICE_BAND) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckNoBoost },
    { .name = "choice_band_locks_move",
      .player = { MON_ITEM(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckChoiceLock },
    { .name = "choice_band_pp_out_struggle",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH }, .item = ITEM_CHOICE_BAND, .pp = { 1, 40, 40, 40 }, .ppSet = 1 } },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .check = CheckChoicePpOut },
    { .name = "choice_band_disabled_struggle",
      .player = { MON_ITEM(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND) },
      .enemy = { M1(SPECIES_DROWZEE, 50, MOVE_DISABLE) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantTackleDisabled, .check = CheckChoiceDisabled },
    { .name = "choice_band_struggle_no_lock",
      .player = { { .species = SPECIES_RATTATA, .level = 50, .moves = { MOVE_TACKLE, MOVE_GROWL, MOVE_NONE, MOVE_NONE }, .item = ITEM_CHOICE_BAND, .pp = { 0, 0, 0, 0 }, .ppSet = 1 } },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckChoiceStruggleNoLock },
    { .name = "choice_band_knock_off_unlocks",
      .player = { MON_ITEM(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND) },
      .enemy = { MON(SPECIES_SABLEYE, 50, MOVE_SPLASH, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 1), T(1, 0) }, .turns = 3, .check = CheckChoiceKnockOffUnlocks },
    { .name = "choice_band_trick_moves_lock",
      .player = { MON_ITEM(SPECIES_ALAKAZAM, 50, MOVE_TRICK, MOVE_PSYCHIC, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND) },
      .enemy = { MON(SPECIES_SNORLAX, 50, MOVE_TACKLE, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckChoiceTrick },
    { .name = "choice_band_switch_resets_lock",
      .player = { MON_ITEM(SPECIES_RATTATA, 50, MOVE_TACKLE, MOVE_GROWL, MOVE_SPLASH, MOVE_SPLASH, ITEM_CHOICE_BAND), M1(SPECIES_PIDGEY, 50, MOVE_SPLASH) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0), T(SC_SWITCH(0), 0), T(1, 0) }, .turns = 4, .check = CheckChoiceSwitchReset },

    // ---- speed items ----
    { .name = "quick_claw_activates",
      .player = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_QUICK_CLAW) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0), T(0, 0) }, .turns = 2, .wantSeed = WantQuickClawOnce, .check = CheckQuickClaw },
    { .name = "quick_claw_does_not_beat_priority",
      .player = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_QUICK_CLAW) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_QUICK_ATTACK) },
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 5, .check = CheckQuickClawPriority },
    { .name = "macho_brace_halves_speed",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_SPLASH, ITEM_MACHO_BRACE) },
      .enemy = { M1(SPECIES_MACHOP, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckMachoBrace },

    // ---- King's Rock / Focus Band / Shell Bell / accuracy ----
    { .name = "kings_rock_flinch",
      .player = { MI(SPECIES_PERSIAN, 50, MOVE_SCRATCH, ITEM_KINGS_ROCK) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantFlinch, .check = CheckKingsRock },
    { .name = "kings_rock_unflagged_move",
      .player = { MI(SPECIES_PERSIAN, 50, MOVE_HYPER_VOICE, ITEM_KINGS_ROCK) },
      .enemy = { M1(SPECIES_STEELIX, 50, MOVE_SPLASH) },   // Hyper Voice is Normal = physical in gen 3: a Defense wall
      .actions = { T(0, 0), T(0, 0), T(0, 0), T(0, 0), T(0, 0) }, .turns = 5, .check = CheckKingsRockUnflagged },
    { .name = "focus_band_holds_on",
      .player = { MHP(SPECIES_RATTATA, 50, MOVE_SPLASH, ITEM_FOCUS_BAND, 5) },
      .enemy = { M1(SPECIES_HITMONLEE, 50, MOVE_SEISMIC_TOSS) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantHungOn, .check = CheckFocusBandHolds },
    { .name = "focus_band_fails",
      .player = { MHP(SPECIES_RATTATA, 50, MOVE_SPLASH, ITEM_FOCUS_BAND, 5) },
      .enemy = { M1(SPECIES_HITMONLEE, 50, MOVE_SEISMIC_TOSS) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantNotHungOn, .check = CheckFocusBandFails },
    { .name = "shell_bell_heals_eighth",
      .player = { MHP(SPECIES_HITMONLEE, 50, MOVE_SEISMIC_TOSS, ITEM_SHELL_BELL, 85) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckShellBell },
    { .name = "shell_bell_full_hp_silent",
      .player = { MI(SPECIES_HITMONLEE, 50, MOVE_SEISMIC_TOSS, ITEM_SHELL_BELL) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckShellBellFull },
    { .name = "shell_bell_minimum_1",
      .player = { MHP(SPECIES_RATTATA, 50, MOVE_TACKLE, ITEM_SHELL_BELL, 50) },
      .enemy = { M1(SPECIES_STEELIX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckShellBellMinimum },
    { .name = "shell_bell_counts_substitute_damage",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SEISMIC_TOSS, ITEM_SHELL_BELL, 85) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SUBSTITUTE) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckShellBellSubstitute },
    { .name = "bright_powder_causes_miss",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_TACKLE) },
      .enemy = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_BRIGHT_POWDER) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantMiss, .check = CheckEvasionItemMiss },
    { .name = "lax_incense_causes_miss",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_TACKLE) },
      .enemy = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LAX_INCENSE) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantMiss, .check = CheckEvasionItemMiss },

    // ---- type-boost items (1.1x, Sea Incense 1.05x) ----
    { .name = "silk_scarf_normal",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_STRENGTH, ITEM_SILK_SCARF) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "charcoal_fire",
      .player = { MI(SPECIES_CHARMELEON, 50, MOVE_FLAMETHROWER, ITEM_CHARCOAL) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "mystic_water_water",
      .player = { MI(SPECIES_WARTORTLE, 50, MOVE_SURF, ITEM_MYSTIC_WATER) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "sea_incense_water_5_percent",
      .player = { MI(SPECIES_WARTORTLE, 50, MOVE_SURF, ITEM_SEA_INCENSE) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "miracle_seed_grass",
      .player = { MI(SPECIES_IVYSAUR, 50, MOVE_MAGICAL_LEAF, ITEM_MIRACLE_SEED) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "magnet_electric",
      .player = { MI(SPECIES_JOLTEON, 50, MOVE_THUNDERBOLT, ITEM_MAGNET) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "never_melt_ice_ice",
      .player = { MI(SPECIES_DEWGONG, 50, MOVE_ICE_BEAM, ITEM_NEVER_MELT_ICE) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "black_belt_fighting",
      .player = { MI(SPECIES_MACHOP, 50, MOVE_BRICK_BREAK, ITEM_BLACK_BELT) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "poison_barb_poison",
      .player = { MI(SPECIES_ARBOK, 50, MOVE_SLUDGE_BOMB, ITEM_POISON_BARB) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "soft_sand_ground",
      .player = { MI(SPECIES_SANDSLASH, 50, MOVE_EARTHQUAKE, ITEM_SOFT_SAND) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "sharp_beak_flying",
      .player = { MI(SPECIES_PIDGEOT, 50, MOVE_WING_ATTACK, ITEM_SHARP_BEAK) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "twisted_spoon_psychic",
      .player = { MI(SPECIES_KADABRA, 50, MOVE_PSYCHIC, ITEM_TWISTED_SPOON) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "silver_powder_bug",
      .player = { MI(SPECIES_BUTTERFREE, 50, MOVE_SILVER_WIND, ITEM_SILVER_POWDER) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "hard_stone_rock",
      .player = { MI(SPECIES_GRAVELER, 50, MOVE_ROCK_THROW, ITEM_HARD_STONE) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "spell_tag_ghost",
      .player = { MI(SPECIES_GENGAR, 50, MOVE_SHADOW_BALL, ITEM_SPELL_TAG) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "dragon_fang_dragon",
      .player = { MI(SPECIES_DRAGONAIR, 50, MOVE_DRAGON_CLAW, ITEM_DRAGON_FANG) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "black_glasses_dark",
      .player = { MI(SPECIES_HOUNDOOM, 50, MOVE_CRUNCH, ITEM_BLACK_GLASSES) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "metal_coat_steel",
      .player = { MI(SPECIES_SKARMORY, 50, MOVE_STEEL_WING, ITEM_METAL_COAT) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "charcoal_wrong_type",
      .player = { MI(SPECIES_CHARMELEON, 50, MOVE_SCRATCH, ITEM_CHARCOAL) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckNoBoost },
    { .name = "silk_scarf_wrong_type",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_THUNDERBOLT, ITEM_SILK_SCARF) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckNoBoost },

    // ---- species-specific items ----
    { .name = "soul_dew_latios_sp_attack",
      .player = { MI(SPECIES_LATIOS, 50, MOVE_PSYCHIC, ITEM_SOUL_DEW) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "soul_dew_wrong_species",
      .player = { MI(SPECIES_ALAKAZAM, 50, MOVE_PSYCHIC, ITEM_SOUL_DEW) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckNoBoost },
    { .name = "soul_dew_latias_sp_defense",
      .player = { M1(SPECIES_ALAKAZAM, 50, MOVE_PSYCHIC) },
      .enemy = { MI(SPECIES_LATIAS, 50, MOVE_SPLASH, ITEM_SOUL_DEW) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDefBoost, .check = CheckDefBoost },
    { .name = "deep_sea_tooth_clamperl",
      .player = { MI(SPECIES_CLAMPERL, 50, MOVE_SURF, ITEM_DEEP_SEA_TOOTH) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "deep_sea_tooth_wrong_species",
      .player = { MI(SPECIES_GOREBYSS, 50, MOVE_SURF, ITEM_DEEP_SEA_TOOTH) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckNoBoost },
    { .name = "deep_sea_scale_clamperl",
      .player = { M1(SPECIES_BUTTERFREE, 50, MOVE_CONFUSION) },
      .enemy = { MI(SPECIES_CLAMPERL, 50, MOVE_SPLASH, ITEM_DEEP_SEA_SCALE) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDefBoost, .check = CheckDefBoost },
    { .name = "light_ball_pikachu",
      .player = { MI(SPECIES_PIKACHU, 50, MOVE_THUNDERBOLT, ITEM_LIGHT_BALL) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "light_ball_raichu_no_boost",
      .player = { MI(SPECIES_RAICHU, 50, MOVE_THUNDERBOLT, ITEM_LIGHT_BALL) },
      .enemy = { M1(SPECIES_CHANSEY, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckNoBoost },
    { .name = "light_ball_physical_no_boost",
      .player = { MI(SPECIES_PIKACHU, 50, MOVE_QUICK_ATTACK, ITEM_LIGHT_BALL) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckNoBoost },
    { .name = "thick_club_marowak",
      .player = { MI(SPECIES_MAROWAK, 50, MOVE_EARTHQUAKE, ITEM_THICK_CLUB) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "thick_club_cubone",
      .player = { MI(SPECIES_CUBONE, 50, MOVE_EARTHQUAKE, ITEM_THICK_CLUB) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantAtkBoost, .check = CheckAtkBoost },
    { .name = "thick_club_wrong_species",
      .player = { MI(SPECIES_SANDSLASH, 50, MOVE_EARTHQUAKE, ITEM_THICK_CLUB) },
      .enemy = { M1(SPECIES_BLASTOISE, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckNoBoost },
    { .name = "metal_powder_ditto",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_TACKLE) },
      .enemy = { MI(SPECIES_DITTO, 50, MOVE_SPLASH, ITEM_METAL_POWDER) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantDefBoost, .check = CheckDefBoost },
    { .name = "metal_powder_transformed_no_boost",
      .player = { M1(SPECIES_SLOWPOKE, 50, MOVE_TACKLE) },
      .enemy = { MI(SPECIES_DITTO, 50, MOVE_TRANSFORM, ITEM_METAL_POWDER) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckNoBoost },
    { .name = "stick_farfetchd_crit",
      .player = { MI(SPECIES_FARFETCHD, 50, MOVE_SLASH, ITEM_STICK) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCrit, .check = CheckCrit },
    { .name = "lucky_punch_chansey_crit",
      .player = { MI(SPECIES_CHANSEY, 50, MOVE_POUND, ITEM_LUCKY_PUNCH) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCrit, .check = CheckCrit },
    { .name = "scope_lens_crit",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_TACKLE, ITEM_SCOPE_LENS) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = WantCrit, .check = CheckCrit },

    // ---- Knock Off / Thief / Trick / Recycle ----
    { .name = "knock_off_removes_item",
      .player = { M1(SPECIES_SABLEYE, 50, MOVE_KNOCK_OFF) },
      .enemy = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS, 200) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckKnockOff },
    { .name = "knock_off_sticky_hold",
      .player = { M1(SPECIES_SABLEYE, 50, MOVE_KNOCK_OFF) },
      .enemy = { { .species = SPECIES_GRIMER, .level = 50, .moves = { MOVE_SPLASH, SPLASH3 }, .item = ITEM_LEFTOVERS, .abilityNum = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckKnockOffStickyHold },
    { .name = "knock_off_no_item",
      .player = { M1(SPECIES_SABLEYE, 50, MOVE_KNOCK_OFF) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckKnockOffNoItem },
    { .name = "knock_off_prevents_berry",
      .player = { M1(SPECIES_SABLEYE, 50, MOVE_KNOCK_OFF) },
      .enemy = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_ORAN_BERRY, 117) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckKnockOffPreventsBerry },
    { .name = "thief_steals",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_THIEF) },
      .enemy = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThief },
    { .name = "thief_while_holding_item",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_THIEF, ITEM_ORAN_BERRY) },
      .enemy = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefHoldingItem },
    { .name = "thief_by_opponent_no_steal",
      .player = { MHP(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS, 200) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_THIEF) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefByOpponent },
    { .name = "thief_sticky_hold",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_THIEF) },
      .enemy = { { .species = SPECIES_GRIMER, .level = 50, .moves = { MOVE_SPLASH, SPLASH3 }, .item = ITEM_LEFTOVERS, .abilityNum = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefStickyHold },
    { .name = "thief_blocked_by_substitute",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_THIEF) },
      .enemy = { MI(SPECIES_PERSIAN, 50, MOVE_SUBSTITUTE, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckThiefSubstitute },
    { .name = "thief_after_being_knocked_off",
      .player = { MON_ITEM(SPECIES_RATTATA, 50, MOVE_SPLASH, MOVE_THIEF, MOVE_SPLASH, MOVE_SPLASH, ITEM_ORAN_BERRY) },
      .enemy = { MON_ITEM(SPECIES_SABLEYE, 50, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckThiefAfterKnockedOff },
    { .name = "trick_swaps_items",
      .player = { MI(SPECIES_ALAKAZAM, 50, MOVE_TRICK, ITEM_TWISTED_SPOON) },
      .enemy = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrick },
    { .name = "trick_gives_item_to_empty_target",
      .player = { MI(SPECIES_ALAKAZAM, 50, MOVE_TRICK, ITEM_TWISTED_SPOON) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickGives },
    { .name = "trick_by_opponent_fails",
      .player = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { MI(SPECIES_ALAKAZAM, 50, MOVE_TRICK, ITEM_TWISTED_SPOON) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickByOpponent },
    { .name = "trick_no_items_fails",
      .player = { M1(SPECIES_ALAKAZAM, 50, MOVE_TRICK) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickFails },
    { .name = "trick_sticky_hold",
      .player = { MI(SPECIES_ALAKAZAM, 50, MOVE_TRICK, ITEM_TWISTED_SPOON) },
      .enemy = { { .species = SPECIES_GRIMER, .level = 50, .moves = { MOVE_SPLASH, SPLASH3 }, .item = ITEM_LEFTOVERS, .abilityNum = 1 } },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckTrickStickyHold },
    { .name = "trick_after_knock_off_fails",
      .player = { MON_ITEM(SPECIES_SABLEYE, 50, MOVE_KNOCK_OFF, MOVE_TRICK, MOVE_SPLASH, MOVE_SPLASH, ITEM_TWISTED_SPOON) },
      .enemy = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .actions = { T(0, 0), T(1, 0) }, .turns = 2, .check = CheckTrickAfterKnockOff },
    { .name = "recycle_restores_eaten_berry",
      .player = { { .species = SPECIES_MR_MIME, .level = 50, .moves = { MOVE_RECYCLE, SPLASH3 }, .item = ITEM_ORAN_BERRY, .hp = 47, .hpSet = 1 } },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(1, 0), T(0, 0) }, .turns = 2, .check = CheckRecycle },
    { .name = "recycle_nothing_used_fails",
      .player = { M1(SPECIES_MR_MIME, 50, MOVE_RECYCLE) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRecycleFails },
    { .name = "recycle_after_knock_off_fails",
      .player = { MON_ITEM(SPECIES_MR_MIME, 50, MOVE_SPLASH, MOVE_RECYCLE, MOVE_SPLASH, MOVE_SPLASH, ITEM_LEFTOVERS) },
      .enemy = { MON(SPECIES_SABLEYE, 50, MOVE_KNOCK_OFF, MOVE_SPLASH, MOVE_SPLASH, MOVE_SPLASH) },
      .actions = { T(0, 0), T(1, 1) }, .turns = 2, .check = CheckRecycleAfterKnockOff },
    { .name = "recycle_while_holding_fails",
      .player = { MI(SPECIES_MR_MIME, 50, MOVE_RECYCLE, ITEM_LEFTOVERS) },
      .enemy = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckRecycleHolding },

    // ---- misc ----
    { .name = "amulet_coin_doubles_money",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_SPLASH, ITEM_AMULET_COIN) },
      .enemy = { M1(SPECIES_SNORLAX, 50, MOVE_SPLASH) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAmuletCoin },
    { .name = "amulet_coin_on_enemy_also_doubles",
      .player = { M1(SPECIES_RATTATA, 50, MOVE_SPLASH) },
      .enemy = { MI(SPECIES_SNORLAX, 50, MOVE_SPLASH, ITEM_AMULET_COIN) },
      .actions = { T(0, 0) }, .turns = 1, .check = CheckAmuletCoin },
    { .name = "smoke_ball_does_not_free_switch",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_SPLASH, ITEM_SMOKE_BALL), M1(SPECIES_PIDGEY, 50, MOVE_SPLASH) },
      .enemy = { M1(SPECIES_GOLBAT, 50, MOVE_MEAN_LOOK) },
      .actions = { T(0, 0), T(SC_SWITCH(1), 0) }, .turns = 2, .check = CheckSmokeBallTrapped },
    { .name = "inert_everstone_lucky_egg",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_STRENGTH, ITEM_EVERSTONE) },
      .enemy = { MI(SPECIES_BLASTOISE, 50, MOVE_SPLASH, ITEM_LUCKY_EGG) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckInert },
    { .name = "inert_exp_share_cleanse_tag",
      .player = { MI(SPECIES_RATTATA, 50, MOVE_STRENGTH, ITEM_EXP_SHARE) },
      .enemy = { MI(SPECIES_BLASTOISE, 50, MOVE_SPLASH, ITEM_CLEANSE_TAG) },
      .actions = { T(0, 0) }, .turns = 1, .wantSeed = CleanHit, .check = CheckInert },
};

SCENARIO_GROUP(held_items, sScenarios)
