#ifndef GUARD_GAME_RULES_H
#define GUARD_GAME_RULES_H

// Custom game rules: badge-based level cap, zero IVs/EVs, trainer resets.
// The level cap table lives in src/game_rules.c.

u8 GetBadgeCount(void);
u8 GetLevelCap(void);
u32 GetLevelCapExp(u16 species);
void ResetAllTrainerFlags(void);
void ResetHealingItems(void);
struct Pokemon;
void ApplyTrainerMonPreStatus(u16 trainerNum, u8 slot, struct Pokemon *mon);

#endif // GUARD_GAME_RULES_H
