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
};

// ── Agent state ─────────────────────────────────────────────────────────────

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
	float PersonalSnapTime         =   2.f;  // sec — drift time toward exact slot after stopping
	float PersonalDriftAmp         =   5.f;  // cm — amplitude of lateral wavering while marching
	float PersonalWaverAmp         =   0.04f;// fraction — speed wavering ±N%
	float PersonalSeparationRadius = 150.f;  // cm — personal space (separation query radius)

	// ── Formation slot info (set by spawner) ────────────────────────────────
	int32 FormationRow  = 0;       // row index (0 = front)
	int32 FormationCol  = 0;       // column index in row
	float CurveOffset   = 0.f;    // organic per-entity offset perpendicular to front line (cm)
};

USTRUCT()
struct BIEDA_TOTAL_WAR_API FAgentCombatFragment : public FMassFragment
{
	GENERATED_BODY()

	// ── Timing ──────────────────────────────────────────────────────────────
	float ReloadDuration     = 15.f;
	float AimDuration        = 2.f;
	float FireDuration       = 0.5f;

	// ── Morale modifiers ────────────────────────────────────────────────────
	float MoraleDrainFire    = 15.f;
	float MoraleDrainLoading = 0.5f;
	float RouteRecoveryRate  = 3.f;
	float PanicThreshold     = 20.f;

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

	// ── Volley coordination (line infantry) ─────────────────────────────────
	EVolleyMode VolleyMode   = EVolleyMode::FreeFire;
	bool bVolleyReady        = false;   // soldier finished loading, waiting for signal
	bool bVolleySignal       = false;   // coordinator says FIRE (set by spawner Tick)
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
	float MoraleBonus    = 1.f;     // morale/s granted to soldiers within MoraleRadius
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
	float ChaseSpeed  = 600.f;   // cm/s — when chasing a straggler or routing soldier

	// Current assignment — soldier being chased
	FMassEntityHandle TargetSoldier;
	bool  bHasTarget = false;

	// Home position in formation (set by spawner on move order)
	FVector FormationPos    = FVector::ZeroVector;
	bool    bHasFormationPos = false;

	// Rally stats
	float RallyMoraleBoost = 5.f;    // morale/s boost to routing soldiers within radius
	float RallyRadius      = 300.f;  // cm
};

// ── Faction ─────────────────────────────────────────────────────────────────

USTRUCT()
struct BIEDA_TOTAL_WAR_API FFactionFragment : public FMassFragment
{
	GENERATED_BODY()
	uint8 TeamId  = 0;   // 0 = player, 1 = enemy, etc.
	uint8 SquadId = 0;   // unique per spawner — binds officer to his soldiers
};
