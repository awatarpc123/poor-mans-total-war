#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleDrummerProcessor.generated.h"

/**
 * Drives the drummer entities each frame.
 *
 * Drummers (doboszowie) march with the line and keep the formation pace. They
 * have no combat or morale role of their own — their job is to relay the
 * officer's move orders by drumbeat (see BattleOrderProcessor, which reads
 * FDrummerFragment::DrumRadius). This processor only handles their movement:
 * walk toward FormationPos at MoveSpeed while farther than the slot threshold.
 */
UCLASS()
class BIEDA_TOTAL_WAR_API UBattleDrummerProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UBattleDrummerProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery DrummerQuery;   // entities with FDrummerFragment
};
