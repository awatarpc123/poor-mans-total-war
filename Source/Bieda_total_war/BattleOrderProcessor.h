#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleOrderProcessor.generated.h"

/**
 * Propagates advance orders through the formation as a wave:
 *   Officer → nearby soldiers (immediate) → their neighbours (+0.3–0.8s per hop)
 * Drummers (doboszowie) act as relay nodes — soldiers within a living drummer's
 * DrumRadius hear the order directly, extending the officer's effective voice
 * range across a wide line.
 * Low-morale soldiers have a chance to ignore the order entirely (transmission noise).
 */
UCLASS()
class BIEDA_TOTAL_WAR_API UBattleOrderProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UBattleOrderProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery OfficerQuery;   // FTransform + FOfficer
	FMassEntityQuery DrummerQuery;   // FTransform + FDrummer  (order relay nodes)
	FMassEntityQuery SoldierQuery;   // FTransform + FAgentState + FMorale + FOrderPropagation

	// Officer voice range — soldiers inside hear the order directly
	static constexpr float OfficerVoiceRadius = 500.f;   // cm

	// Peer-to-peer propagation range — immediate grid neighbours only
	static constexpr float PropagationRadius  = 200.f;   // cm
};
