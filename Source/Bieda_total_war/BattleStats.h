#pragma once

#include "Stats/Stats.h"

// Custom stat group for the battle simulation. View the per-system breakdown
// in-game with the console command:  stat BiedaTotalWar
// Each gameplay processor / spawner tick-helper scopes its work under one of
// the cycle counters declared in the individual .cpp files, so you can see at a
// glance which system dominates the game thread at high agent counts.
DECLARE_STATS_GROUP(TEXT("BiedaTotalWar"), STATGROUP_Bieda, STATCAT_Advanced);
