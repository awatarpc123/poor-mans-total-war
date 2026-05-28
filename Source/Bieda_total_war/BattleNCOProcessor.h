#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleNCOProcessor.generated.h"

/**
 * NCO (Podoficer) AI — chases stragglers to deliver orders, helps rally routing soldiers.
 *
 * Runs AFTER BattleOrderProcessor (reads soldier order state after propagation).
 * Runs BEFORE BattleStateProcessor.
 *
 * Pass 0 — snapshot soldiers: positions, order state, agent state, squad, entity handles
 * Pass 1 — for each NCO: find target, move, deliver order / rally
 */
UCLASS()
class BIEDA_TOTAL_WAR_API UBattleNCOProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UBattleNCOProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery NCOQuery;
	FMassEntityQuery SoldierSnapshotQuery;
};
