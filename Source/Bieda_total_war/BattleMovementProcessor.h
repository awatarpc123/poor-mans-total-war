#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "MassEntityQuery.h"
#include "MassEntityManager.h"
#include "BattleMovementProcessor.generated.h"

UCLASS()
class BIEDA_TOTAL_WAR_API UBattleMovementProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UBattleMovementProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery MovementQuery;

	static constexpr float MoveSpeed     = 200.f;
	static constexpr float RoutingSpeed  = 320.f;
	static constexpr float StopDistance  = 100.f;
};
