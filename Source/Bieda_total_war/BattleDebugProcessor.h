#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleDebugProcessor.generated.h"

/** Is battle debug drawing currently enabled? Backed by the `bieda.Debug`
 *  console variable (0=off default, 1=on). Used by every system that issues
 *  DrawDebug* calls so they can be silenced together for perf/builds. */
BIEDA_TOTAL_WAR_API bool BiedaDebugDrawEnabled();

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
