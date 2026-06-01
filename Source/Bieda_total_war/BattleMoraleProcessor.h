#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleMoraleProcessor.generated.h"

/**
 * Applies social morale effects based on nearby agents:
 *   ROUTING neighbor within 1500cm  → -3/s morale drain  (panic contagion)
 *   DEAD    neighbor within  500cm  → -2/s morale drain  (corpse shock)
 *   Stable  neighbor within 1000cm  → +0.5/s morale bonus (unit cohesion)
 *
 * Uses a two-pass snapshot approach: O(n²), suitable for up to ~1000 agents.
 * For 10k+ agents this should be replaced with a spatial hash grid.
 */
UCLASS()
class BIEDA_TOTAL_WAR_API UBattleMoraleProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UBattleMoraleProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery MoraleQuery;
	FMassEntityQuery OfficerQuery;   // read officer aura data

	// Radii (cm)
	static constexpr float RoutingRadius = 1000.f;   // panic contagion reach (was 1500 — 15m was too far)
	static constexpr float DeadRadius    =  500.f;
	static constexpr float StableRadius  = 1000.f;

	// Panic contagion critical mass (fraction of the squad already breaking).
	// Below ContagionFloor, fear doesn't spread at all (lone panicker is steadied
	// by his mates); it ramps to full strength by ContagionFull. Fraction-based,
	// so identical behaviour at any squad size.
	static constexpr float ContagionFloor = 0.15f;   // <15% breaking → no spread
	static constexpr float ContagionFull  = 0.40f;   // ≥40% breaking → full spread

	// Per-neighbor rates (/s) — Routing/Shaken now distance-weighted (see .cpp)
	static constexpr float RoutingDrainRate = 3.f;
	static constexpr float DeadDrainRate    = 2.f;
	static constexpr float ShakenDrainRate  = 1.5f;   // wavering neighbour unsettles you
	static constexpr float StableBonusRate  = 0.5f;

	// Caps on total effect (/s)
	static constexpr float MaxRoutingDrain  = 6.f;
	static constexpr float MaxDeadDrain     = 4.f;
	static constexpr float MaxShakenDrain   = 3.f;
	static constexpr float MaxStableBonus   = 3.f;
};
