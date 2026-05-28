#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleOfficerProcessor.generated.h"

/**
 * Drives the officer entity each frame:
 *  - Moves toward the nearest ROUTING soldier (rally behavior)
 *  - Otherwise follows the formation centroid
 *  - Drains own morale when surrounded by routing soldiers
 *  - Sets bJustDied for one frame on death (triggers morale cascade in MoraleProcessor)
 */
UCLASS()
class BIEDA_TOTAL_WAR_API UBattleOfficerProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UBattleOfficerProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery OfficerQuery;   // entities with FOfficerFragment
	FMassEntityQuery SoldierQuery;   // entities with FAgentStateFragment (soldiers only)
};
