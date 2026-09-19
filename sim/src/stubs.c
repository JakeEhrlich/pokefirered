// Stubs for graphics / sound / save / link / menu functions referenced by the ported battle code.
// None of these affect battle mechanics.
#include <string.h>
#include "global.h"
#include "gflib.h"
#include "task.h"
#include "main.h"
#include "link.h"
#include "battle.h"
#include "battle_interface.h"
#include "battle_message.h"
#include "battle_controllers.h"
#include "battle_setup.h"
#include "pokemon.h"
#include "pokemon_storage_system.h"
#include "pokedex.h"
#include "money.h"
#include "event_data.h"
#include "strings.h"
#include "naming_screen.h"
#include "evolution_scene.h"
#include "pokemon_summary_screen.h"
#include "pokemon_icon.h"
#include "reshow_battle_screen.h"
#include "sim_globals.h"

// ---- global data ----
struct Task gTasks[NUM_TASKS];
struct Sprite gSprites[MAX_SPRITES + 1];
struct PaletteFadeControl gPaletteFade;
u16 gPlttBufferFaded[PLTT_BUFFER_SIZE];
struct Main gMain = { .inBattle = TRUE };
struct LinkPlayer gLinkPlayers[MAX_RFU_PLAYERS];
const u8 gGameVersion = VERSION_FIRE_RED;
const u8 gGameLanguage = LANGUAGE_ENGLISH;
u8 gDisplayedStringBattle[300];
u16 gSpecialVar_MonBoxId;
u16 gSpecialVar_MonBoxPos;
u16 gSpecialVar_0x8004;
u16 gSpecialVar_0x8005;
u16 gSpecialVar_0x8006;
u16 gBattle_BG0_X, gBattle_BG0_Y, gBattle_BG1_X, gBattle_BG1_Y, gBattle_BG2_X, gBattle_BG2_Y, gBattle_BG3_X, gBattle_BG3_Y;

static struct SaveBlock1 sSaveBlock1;
static struct SaveBlock2 sSaveBlock2 = { .playerName = { 0xBB, 0xC3, 0xC7, 0xFF }, .optionsBattleStyle = 1 /* SET */ };
struct SaveBlock1 *gSaveBlock1Ptr = &sSaveBlock1;
struct SaveBlock2 *gSaveBlock2Ptr = &sSaveBlock2;

const struct OamData gDummyOamData = {0};
const union AnimCmd *const gDummySpriteAnimTable[] = { NULL };
const union AffineAnimCmd *const gDummySpriteAffineAnimTable[] = { NULL };
const union AffineAnimCmd *const gAffineAnims_BattleSpritePlayerSide[] = { NULL };
const union AffineAnimCmd *const gAffineAnims_BattleSpriteOpponentSide[] = { NULL };
const struct SpriteFrameImage gBattlerPicTable_PlayerLeft[1] = {{0}};
const struct SpriteFrameImage gBattlerPicTable_PlayerRight[1] = {{0}};
const struct SpriteFrameImage gBattlerPicTable_OpponentLeft[1] = {{0}};
const struct SpriteFrameImage gBattlerPicTable_OpponentRight[1] = {{0}};

const u8 gText_Sleep[] = _("sleep");
const u8 gText_Poison[] = _("poison");
const u8 gText_Burn[] = _("burn");
const u8 gText_Paralysis[] = _("paralysis");
const u8 gText_Ice[] = _("ice");
const u8 gText_Confusion[] = _("confusion");
const u8 gText_Love[] = _("love");
const u8 gText_Ghost[] = _("GHOST");
const u8 gText_EggNickname[] = _("EGG");
const u8 gText_BadEgg[] = _("BAD EGG");
const u8 gText_PkmnsXPreventsSwitching[] = _("{STR_VAR_1}'s {STR_VAR_2}\nprevents switching!\p");
const u8 gText_DefendersStatRose[] = _("{B_DEF_NAME_WITH_PREFIX}'s {B_BUFF1}\nrose!");
const u8 gText_BattleYesNoChoice[] = _("{PALETTE 5}Yes\nNo");
const u8 gBattleText_Rose[] = _("rose");
const u8 gBattleText_MistShroud[] = _("shrouded in MIST!");
const u8 gBattleText_GetPumped[] = _("is getting pumped!");
static const u8 sText_HP[] = _("HP");
static const u8 sText_Attack[] = _("ATTACK");
static const u8 sText_Defense[] = _("DEFENSE");
static const u8 sText_Speed[] = _("SPEED");
static const u8 sText_SpAtk[] = _("SP. ATK");
static const u8 sText_SpDef[] = _("SP. DEF");
static const u8 sText_Accuracy[] = _("accuracy");
static const u8 sText_Evasion[] = _("evasiveness");
const u8 *const gStatNamesTable[NUM_BATTLE_STATS] = { sText_HP, sText_Attack, sText_Defense, sText_Speed, sText_SpAtk, sText_SpDef, sText_Accuracy, sText_Evasion };

// ---- BIOS ----
void CpuSet(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFF;
    if (control & (1 << 26))
    {
        u32 *d = dest;
        const u32 *s = src;
        if (control & (1 << 24))
            while (count--) *d++ = *s;
        else
            while (count--) *d++ = *s++;
    }
    else
    {
        u16 *d = dest;
        const u16 *s = src;
        if (control & (1 << 24))
            while (count--) *d++ = *s;
        else
            while (count--) *d++ = *s++;
    }
}

// ---- graphics / sound / windows: no-ops ----
void ShowBg(u8 bg) {}
void SetBgAttribute(u8 bg, u8 attributeId, u8 value) {}
void SetVBlankCallback(IntrCallback callback) {}
void VBlankCB_Battle(void) {}
void PutWindowTilemap(u8 windowId) {}
void ClearWindowTilemap(u8 windowId) {}
void CopyWindowToVram(u8 windowId, u8 mode) {}
void CopyToWindowPixelBuffer(u8 windowId, const void *src, u16 size, u16 tileOffset) {}
void CopyToBgTilemapBufferRect_ChangePalette(u8 bg, const void *src, u8 destX, u8 destY, u8 rectWidth, u8 rectHeight, u8 palette) {}
void CopyBgTilemapBufferToVram(u8 bg) {}
void FreeAllWindowBuffers(void) {}
u16 AddTextPrinter(struct TextPrinterTemplate *template, u8 speed, void (*callback)(struct TextPrinterTemplate *, u16)) { return 0; }
void LoadPalette(const void *src, u16 offset, u16 size) {}
u16 LoadSpriteSheet(const struct SpriteSheet *sheet) { return 0; }
u8 LoadSpritePalette(const struct SpritePalette *palette) { return 0; }
void FreeSpriteTilesByTag(u16 tag) {}
void FreeSpritePaletteByTag(u16 tag) {}
u8 CreateSprite(const struct SpriteTemplate *template, s16 x, s16 y, u8 subpriority) { return 0; }
void DestroySprite(struct Sprite *sprite) {}
bool8 BeginNormalPaletteFade(u32 selectedPalettes, s8 delay, u8 startY, u8 targetY, u16 blendColor) { return FALSE; }
void BeginFastPaletteFade(u8 submode) {}
bool8 IsDma3ManagerBusyWithBgCopy(void) { return FALSE; }
void PlaySE(u16 songNum) {}
void PlayBGM(u16 songNum) {}
bool8 IsFanfareTaskInactive(void) { return TRUE; }
void HandleLowHpMusicChange(struct Pokemon *mon, u8 battlerId) {}
void BattleStopLowHpSound(void) {}
void InitBattleBgsVideo(void) {}
void LoadBattleTextboxAndBackground(void) {}
void ClearBattleAnimationVars(void) {}
void ClearTemporarySpeciesSpriteData(u8 battlerId, bool8 dontClearSubstitute) {}
const u8 *GetMonIconPtr(u16 species, u32 personality, u32 frameNo) { return NULL; }
const u16 *GetValidMonIconPalettePtr(u16 species) { return NULL; }
u16 CreateMonPicSprite_HandleDeoxys(u16 species, u32 otId, u32 personality, bool8 isFrontPic, s16 x, s16 y, u8 paletteSlot, u16 paletteTag) { return 0; }
void BattlePutTextOnWindow(const u8 *text, u8 windowId) {}
void ReshowBattleScreenAfterMenu(void) {}
void ShowSelectMovePokemonSummaryScreen(struct Pokemon *mons, u8 monIndex, u8 maxMonIndex, void (*callback)(void), u16 move) {}
void DrawLevelUpWindowPg1(u16 windowId, u16 *statsBefore, u16 *statsAfter, u8 bgClr, u8 fgClr, u8 shdwClr) {}
void DrawLevelUpWindowPg2(u16 windowId, u16 *currStats, u8 bgClr, u8 fgClr, u8 shdwClr) {}
void GetMonLevelUpWindowStats(struct Pokemon *mon, u16 *currStats) {}
u8 GetMoveSlotToReplace(void) { return 0; }
void DoNamingScreen(u8 templateNum, u8 *destBuffer, u16 monSpecies, u16 monGender, u32 monPersonality, MainCallback returnCallback) {}
void BeginEvolutionScene(struct Pokemon *mon, u16 speciesToEvolve, bool8 canStopEvo, u8 partyId) {}
void BattleMainCB2(void) {}
void BattleControllerDummy(void) {}
void SetControllerToPlayer(void) {}
void SetControllerToOpponent(void) {}
void SetControllerToSafari(void) {}
void SetControllerToPokedude(void) {}
void SetControllerToOakOrOldMan(void) {}
bool8 BtlCtrl_OakOldMan_TestState2Flag(u8 flag) { return FALSE; }

// ---- strings ----
u32 BattleStringExpandPlaceholdersToDisplayedString(const u8 *src) { return 0; }
u32 BattleStringExpandPlaceholders(const u8 *src, u8 *dst) { dst[0] = EOS; return 0; }
u8 *GetMonNickname(struct Pokemon *mon, u8 *dest) { GetMonData(mon, MON_DATA_NICKNAME, dest); dest[POKEMON_NAME_LENGTH] = EOS; return dest; }
u8 GetScaledHPFraction(s16 hp, s16 maxhp, u8 scale)
{
    u8 result = hp * scale / maxhp;
    if (result == 0 && hp > 0)
        result = 1;
    return result;
}

// ---- save / pokedex / storage / link ----
u8 StorageGetCurrentBox(void) { return 0; }
bool8 ShouldShowBoxWasFullMessage(void) { return FALSE; }
void SetPCBoxToSendMon(u8 boxId) {}
u8 GetPCBoxToSendMon(void) { return 0; }
u8 *GetBoxNamePtr(u8 boxNumber) { return NULL; }
struct BoxPokemon *GetBoxedMonPtr(u8 boxId, u8 boxPosition) { return NULL; }
u32 GetBoxMonDataAt(u8 boxId, u8 boxPosition, s32 request) { return 0; }
bool32 IsNationalPokedexEnabled(void) { return FALSE; }
s8 GetSetPokedexFlag(u16 nationalDexNo, u8 caseID) { return 0; }
u8 DexScreen_RegisterMonToPokedex(u16 species) { return 0; }
extern const u16 gSimPokedexHeights[];
extern const u16 gSimPokedexWeights[];
u16 GetPokedexHeightWeight(u16 dexNum, u8 data) // pokedex.c
{
    switch (data)
    {
    case 0: return gSimPokedexHeights[dexNum];
    case 1: return gSimPokedexWeights[dexNum];
    default: return 1;
    }
}
void IncrementGameStat(u8 index) {}
void AddMoney(u32 *moneyPtr, u32 toAdd) {}
u32 ComputeWhiteOutMoneyLoss(void) { return 0; }
u8 GetTrainerBattleMode(void) { return 0; }
u16 GetRivalBattleFlags(void) { return 0; }
u8 GetMultiplayerId(void) { return 0; }
u8 GetLinkPlayerCount(void) { return 0; }

// ---- berries / link / misc ----
#include "berry.h"
static const struct Berry sDummyBerry = { .name = _("ENIGMA"), .firmness = 1 };
bool32 IsEnigmaBerryValid(void) { return FALSE; }
const struct Berry *GetBerryInfo(u8 berry) { return &sDummyBerry; }
u8 ItemIdToBerryType(u16 item) { return 1; }
u16 gBlockRecvBuffer[MAX_RFU_PLAYERS][BLOCK_BUFFER_SIZE / 2];
const u8 gExpandedPlaceholder_Empty[] = _("");
const u8 gExpandedPlaceholder_Kun[] = _("");
const u8 gExpandedPlaceholder_Chan[] = _("");
const u8 gExpandedPlaceholder_Red[] = _("RED");
const u8 gExpandedPlaceholder_Green[] = _("GREEN");
const u8 gExpandedPlaceholder_Ruby[] = _("RUBY");
const u8 gExpandedPlaceholder_Aqua[] = _("AQUA");
const u8 gExpandedPlaceholder_Magma[] = _("MAGMA");
const u8 gExpandedPlaceholder_Archie[] = _("ARCHIE");
const u8 gExpandedPlaceholder_Maxie[] = _("MAXIE");
const u8 gExpandedPlaceholder_Kyogre[] = _("KYOGRE");
const u8 gExpandedPlaceholder_Groudon[] = _("GROUDON");
void SpriteCB_EnemyMon(struct Sprite *sprite) {}
void SpriteCB_AllyMon(struct Sprite *sprite) {}
