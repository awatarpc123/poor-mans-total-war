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

// ── Game speed (slow-motion / fast-forward) ──────────────────────────────────
// A simulation time multiplier applied to every battle processor's delta time:
// the battle runs slower or faster while the CAMERA stays at normal speed. Steps
// through a fixed set {0.25, 0.5, 1, 2, 4}. Independent of pause (when paused the
// processors early-out before using DT, so the scale is irrelevant there).
//   StepBattleSimTimeScale(+1) → faster,  (-1) → slower.
// Backed by the `bieda.TimeScale` console variable.
BIEDA_TOTAL_WAR_API float BattleSimTimeScale();
BIEDA_TOTAL_WAR_API void  StepBattleSimTimeScale(int32 Dir);
