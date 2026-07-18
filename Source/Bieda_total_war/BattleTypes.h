#pragma once

#include "CoreMinimal.h"
#include "MassEntityTypes.h"
#include "MassEntityHandle.h"
#include "BattleTypes.generated.h"

// ── Unit type ───────────────────────────────────────────────────────────────

UENUM(BlueprintType)
enum class EUnitType : uint8
{
	Militia       UMETA(DisplayName = "Militia"),
	LineInfantry  UMETA(DisplayName = "Line Infantry"),
};

UENUM(BlueprintType)
enum class EVolleyMode : uint8
{
	FreeFire     UMETA(DisplayName = "Free Fire"),
	SquadVolley  UMETA(DisplayName = "Squad Volley"),
	RankFire     UMETA(DisplayName = "Rank Fire"),
	Countermarch UMETA(DisplayName = "Countermarch Fire"),
};

// ── Fatigue level (ETW: 6 levels, scale 0-28800) ────────────────────────────

UENUM(BlueprintType)
enum class EFatigueLevel : uint8
{
	Fresh     UMETA(DisplayName = "Fresh"),
	Active    UMETA(DisplayName = "Active"),
	Winded    UMETA(DisplayName = "Winded"),
	Tired     UMETA(DisplayName = "Tired"),
	VeryTired UMETA(DisplayName = "Very Tired"),
	Exhausted UMETA(DisplayName = "Exhausted"),
};

// ── Agent state (ETW-style extended) ────────────────────────────────────────

UENUM(BlueprintType)
enum class EAgentState : uint8
{
	HOLDING   UMETA(DisplayName = "Holding"),
	LOADING   UMETA(DisplayName = "Loading"),
	AIMING    UMETA(DisplayName = "Aiming"),
	FIRING    UMETA(DisplayName = "Firing"),
	ADVANCING UMETA(DisplayName = "Advancing"),
	MELEE     UMETA(DisplayName = "Melee"),
	ROUTING   UMETA(DisplayName = "Routing"),
	RALLYING  UMETA(DisplayName = "Rallying"),
	SHAKEN    UMETA(DisplayName = "Shaken"),
	WAVERING  UMETA(DisplayName = "Wavering"),
	STEADY    UMETA(DisplayName = "Steady"),
	PINNED    UMETA(DisplayName = "Pinned"),
	DEAD      UMETA(DisplayName = "Dead"),
};

USTRUCT()
struct BIEDA_TOTAL_WAR_API FAgentStateFragment : public FMassFragment
{
	GENERATED_BODY()
	EAgentState State = EAgentState::HOLDING;
	float StateTimer = 0.f;
};

USTRUCT()
struct BIEDA_TOTAL_WAR_API FMoraleFragment : public FMassFragment
{
	GENERATED_BODY()
	float Morale = 80.f;
};

USTRUCT()
struct BIEDA_TOTAL_WAR_API FOrderFragment : public FMassFragment
{
	GENERATED_BODY()
	FVector TargetPosition = FVector::ZeroVector;
	bool bHasTarget = false;

	/** World position to face when stationary (set by engagement system). */
	FVector FaceTarget = FVector::ZeroVector;
	bool bHasFaceTarget = false;
};

USTRUCT()
struct BIEDA_TOTAL_WAR_API FAgentVelocityFragment : public FMassFragment
{
	GENERATED_BODY()
	FVector Velocity = FVector::ZeroVector;
	float NoiseSeed  = 0.f;    // random per entity (0–1), drives march wavering

	// ── Unit type + formation movement ──────────────────────────────────────
	EUnitType UnitType   = EUnitType::Militia;
	float MarchSpeed     = 200.f;   // cm/s — orderly walk (UI: Marsz)
	float RunSpeed       = 400.f;   // cm/s — fast advance (UI: Bieg)
	float CatchUpSpeed   = 600.f;   // cm/s — straggler sprint (auto, NCO-driven)
	bool  bForceRun      = false;   // UI state: false=Marsz, true=Bieg
	bool  bIsStraggler   = false;   // Set true when soldier falls far behind formation

	// ── Per-soldier "personality" — randomized once at spawn, sticks for life ──
	// These break the uniform-robot look without making the line look like chaos.
	float PersonalSlotTolerance    = 100.f;  // cm — how close to slot before he "stops"
	float PersonalSnapTime         =   2.f;  // sec — drift time toward final resting pos
	float PersonalDriftAmp         =   5.f;  // cm — amplitude of lateral wavering while marching
	float PersonalWaverAmp         =   0.04f;// fraction — speed wavering ±N%
	float PersonalSeparationRadius = 150.f;  // cm — personal space (separation query radius)

	// 2D offset added to TargetPosition when soldier comes to rest. Makes each
	// soldier consistently stop at the same offset from their slot — no amount
	// of re-orders will collapse the line into a perfect grid.
	FVector PersonalFinalOffset    = FVector::ZeroVector;

	// ── Formation slot info (set by spawner) ────────────────────────────────
	int32  FormationRow = 0;       // rank (0 = front)
	int32  FormationCol = 0;       // file (0 = leftmost)
	uint16 SlotIndex    = 0;       // index into pre-computed formation slot array (spawner-owned)
	float   CurveOffset = 0.f;    // organic per-entity offset perpendicular to front line (cm)

	// ── Routing flee direction (latched once on entering ROUTING) ────────────
	// Captured at the moment of panic so it stays stable (fidget can't corrupt
	// it) and always points AWAY from the enemy.  Zero = not yet latched.
	FVector FleeDirection = FVector::ZeroVector;
};

USTRUCT()
struct BIEDA_TOTAL_WAR_API FAgentCombatFragment : public FMassFragment
{
	GENERATED_BODY()

	// ── Timing ──────────────────────────────────────────────────────────────
	float ReloadDuration     = 15.f;
	float AimDuration        = 2.f;
	float FireDuration       = 0.5f;

	// ── Morale modifiers (ETW multi-tier) ───────────────────────────────────
	float MoraleDrainFire     = 15.f;
	float MoraleDrainLoading  = 0.5f;
	float RouteRecoveryRate   = 1.2f;
	float PanicThreshold      = 20.f;   // < this → ROUTING
	float ShakenThreshold     = 25.f;   // < this → SHAKEN (heavy, holds fire)
	float WaverThreshold      = 40.f;   // < this → WAVERING (reduced fire; hysteresis: recovers to HOLDING at +10)
	float ShakenRecover       = 35.f;   // SHAKEN→WAVERING at this (hysteresis)
	float WaverRecover        = 50.f;   // WAVERING→HOLDING at this

	// ── Health ──────────────────────────────────────────────────────────────
	float HP = 100.f;

	// ── Targeting ───────────────────────────────────────────────────────────
	float FireRange          = 5000.f;   // cm — musket effective range (~50 m)
	float Accuracy           = 0.15f;    // hit probability per shot (0.0–1.0)

	/**
	 * Half-angle of the vision cone in degrees.
	 *   90°  → semicircle  (180° total FOV) — default line formation
	 *  180°  → full circle (360° total FOV) — e.g. square formation (czworobok)
	 *
	 * Future formation types can simply change this value.
	 */
	float VisionHalfAngleDeg = 90.f;

	FMassEntityHandle TargetEntity;          // current aim target (valid during AIMING)
	bool bHasAcquiredTarget  = false;        // true when a valid target is locked

	// ── Unit type (copied from spawner) — combat reads it for fire-spread width:
	//    line infantry scatter over a wider candidate pool than militia. ───────
	EUnitType UnitType       = EUnitType::Militia;

	// ── Volley coordination (line infantry) ─────────────────────────────────
	EVolleyMode VolleyMode   = EVolleyMode::FreeFire;
	bool bVolleyReady        = false;   // soldier finished loading, waiting for signal
	bool bVolleySignal       = false;   // coordinator says FIRE (set by spawner Tick)

	// Countermarch fire: set the instant this soldier finishes a shot (FIRING
	// duration elapsed) while VolleyMode==Countermarch. Consumed once per frame
	// by ABattleSpawnerActor::UpdateCountermarch(), which — with access to the
	// whole squad's entity list — reassigns this soldier's FormationRow/Col
	// slot to the rear of their file and sends them there (ADVANCING), then
	// clears the flag. Kept here rather than computed inline in
	// BattleStateProcessor because that processor only sees one entity at a
	// time and can't do the whole-file rank shuffle by itself.
	bool bJustFiredCountermarch = false;

	// Musket starts LOADED at spawn — a unit marches into battle with a charged
	// piece, so its first shot skips the long reload (HOLDING → AIMING straight
	// away). Cleared after the first shot; every later cycle reloads normally.
	bool bMusketLoaded       = true;

	// ── Melee (HN system — ETW HitNumber → XHolds → knockback/knockdown) ─────
	bool  bInMeleeContact   = false;   // set by CombatProcessor: enemy within MeleeRange
	bool  bPrevMeleeContact = false;   // last frame's value — for first-contact charge shock
	float MeleeTimer        = 0.f;     // time since last melee hit

	// ETW unit_stats_land values (copied from spawner)
	float MeleeAttack       = 8.f;     // attack skill (vs defense for HN calc)
	float MeleeDefense      = 8.f;     // defense skill
	float MeleeChargeBonus  = 4.f;     // added to attack when charging (bForceRun + bInMeleeContact)

	// HN result cached by CombatProcessor for knockback evaluation
	float  LastKnockback    = 0.f;     // cm to push target
};

// ── Order propagation ────────────────────────────────────────────────────────

USTRUCT()
struct BIEDA_TOTAL_WAR_API FOrderPropagationFragment : public FMassFragment
{
	GENERATED_BODY()

	bool  bOrderReceived  = false;  // soldier heard the advance order
	bool  bOrderExecuted  = false;  // soldier has started moving (can now propagate)
	bool  bOrderIgnored   = false;  // transmission noise — soldier skips this order
	float ExecutionDelay  = 0.f;    // random delay before acting (s), set on receipt
	float ExecutionTimer  = 0.f;    // elapsed time since order received (s)
};

// ── Officer ─────────────────────────────────────────────────────────────────

USTRUCT()
struct BIEDA_TOTAL_WAR_API FOfficerFragment : public FMassFragment
{
	GENERATED_BODY()

	float MoraleRadius   = 500.f;   // aura radius (cm) — morale bonus to nearby soldiers
	float MoraleBonus    = 4.f;     // morale/s granted to soldiers within MoraleRadius (ETW: inspired=+6)
	float MoveSpeed      = 200.f;   // cm/s — keeps the formation pace (synced by spawner)
	float MoraleDrainCap = 10.f;    // max morale/s drained from nearby routing soldiers

	bool  bIsAlive  = true;
	bool  bJustDied = false;        // true for exactly 1 frame after death (triggers cascade)

	// ── Formation target (set by spawner on move order) ─────────────────────
	FVector FormationFrontPos   = FVector::ZeroVector;   // front-center of formation
	bool    bHasFormationTarget = false;
};

// ── NCO (Podoficer) ────────────────────────────────────────────────────────

USTRUCT()
struct BIEDA_TOTAL_WAR_API FNCOFragment : public FMassFragment
{
	GENERATED_BODY()

	bool  bIsAlive    = true;
	float MoveSpeed   = 200.f;   // cm/s — formation pace when returning to FormationPos
	float ChaseSpeed  = 450.f;   // cm/s — when chasing a straggler or routing soldier

	// Current assignment — soldier being chased
	FMassEntityHandle TargetSoldier;
	bool  bHasTarget = false;

	// Home position in formation (set by spawner on move order)
	FVector FormationPos    = FVector::ZeroVector;
	bool    bHasFormationPos = false;

	// Rally stats
	float RallyMoraleBoost = 2.5f;   // morale/s boost to routing/shaken soldiers within radius
	float RallyRadius      = 300.f;  // cm
};

// ── Drummer (Dobosz) ─────────────────────────────────────────────────────────

USTRUCT()
struct BIEDA_TOTAL_WAR_API FDrummerFragment : public FMassFragment
{
	GENERATED_BODY()

	bool  bIsAlive  = true;
	float MoveSpeed = 200.f;   // cm/s — formation pace (synced by spawner via SetForceRun)

	/** Drumbeat "voice" radius (cm). Soldiers of the same squad within this range
	 *  hear move orders directly — the drummer relays the officer's command.
	 *  Consumed by BattleOrderProcessor. */
	float DrumRadius = 600.f;

	// Home position in formation (set by spawner on move order)
	FVector FormationPos     = FVector::ZeroVector;
	bool    bHasFormationPos = false;
};

// ── Fatigue (ETW scale: 0–28800 points, 6 levels) ───────────────────────────

USTRUCT()
struct BIEDA_TOTAL_WAR_API FFatigueFragment : public FMassFragment
{
	GENERATED_BODY()
	/** Fatigue points: 0 = fresh, 28800 = exhausted (ETW scale).
	 *  Accumulates per-activity (running=8/s, combat=10/s, etc.)
	 *  Decays on idle (-8/s). */
	int32 FatiguePoints = 0;

	/** Current discrete level (Fresh/Active/Winded/...). Updated each frame
	 *  from FatiguePoints threshold lookup — avoids branching in queries. */
	EFatigueLevel Level = EFatigueLevel::Fresh;

	/** Per-unit resistance bonus (from unit_stats_land). -1 = tire slower,
	 *  +1 = tire faster. ETW: fatigue_resistant_delta compensates unit stat. */
	int8 Resistance = 0;
};

// ── Faction ─────────────────────────────────────────────────────────────────

USTRUCT()
struct BIEDA_TOTAL_WAR_API FFactionFragment : public FMassFragment
{
	GENERATED_BODY()
	uint8 TeamId  = 0;   // 0 = player, 1 = enemy, etc.
	uint8 SquadId = 0;   // unique per spawner — binds officer to his soldiers
};
