#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleStateProcessor.generated.h"

UCLASS()
class BIEDA_TOTAL_WAR_API UBattleStateProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UBattleStateProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery StateQuery;
};
