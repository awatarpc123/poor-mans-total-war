#pragma once

#include "CoreMinimal.h"

// ── Tactical pause / time control ────────────────────────────────────────────
// "Active pause" (Total War style): the SIMULATION freezes — every Mass battle
// processor early-outs — but the camera, HUD and order-issuing stay live, so the
// player can pan around and queue move/attack orders while time is stopped. The
// orders just write target data into fragments; with the processors paused the
// soldiers don't act on it until time resumes.
//
// Backed by the `bieda.Pause` console variable (0=running, 1=paused) and the
// keyboard toggle (Space). Each simulation processor's Execute() starts with:
//     if (BattleSimPaused()) return;
// Camera/HUD/visualization deliberately do NOT check it.
BIEDA_TOTAL_WAR_API bool  BattleSimPaused();
BIEDA_TOTAL_WAR_API void  SetBattleSimPaused(bool bPaused);
BIEDA_TOTAL_WAR_API void  ToggleBattleSimPaused();
