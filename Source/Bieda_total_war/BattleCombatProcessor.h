#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleCombatProcessor.generated.h"

/**
 * Handles target acquisition (vision cone) and musket fire / damage.
 *
 * Runs AFTER BattleStateProcessor (reads current state)
 *       BEFORE BattleOfficerProcessor.
 *
 * Pass 0 — snapshot positions, teams, states, entity handles
 * Pass 1 — AIMING: find closest enemy in range + vision cone, face target
 *           FIRING: roll accuracy, queue damage event
 * Pass 2 — apply queued damage, draw debug shot lines
 */
UCLASS()
class BIEDA_TOTAL_WAR_API UBattleCombatProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UBattleCombatProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery CombatQuery;
};
