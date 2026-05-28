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
	static constexpr float RoutingRadius = 1500.f;
	static constexpr float DeadRadius    =  500.f;
	static constexpr float StableRadius  = 1000.f;

	// Per-neighbor rates (/s)
	static constexpr float RoutingDrainRate = 3.f;
	static constexpr float DeadDrainRate    = 2.f;
	static constexpr float StableBonusRate  = 0.5f;

	// Caps on total effect (/s)
	static constexpr float MaxRoutingDrain  = 6.f;
	static constexpr float MaxDeadDrain     = 4.f;
	static constexpr float MaxStableBonus   = 3.f;
};
