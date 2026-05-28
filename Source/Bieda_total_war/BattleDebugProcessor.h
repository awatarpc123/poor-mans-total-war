#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleDebugProcessor.generated.h"

UCLASS()
class BIEDA_TOTAL_WAR_API UBattleDebugProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UBattleDebugProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery DebugQuery;
	FMassEntityQuery OfficerDebugQuery;
	FMassEntityQuery NCODebugQuery;
};
