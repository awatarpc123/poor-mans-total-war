#include "BattleDrummerProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleSimControl.h"
#include "BattleStats.h"

DECLARE_CYCLE_STAT(TEXT("Drummer"), STAT_BiedaDrummer, STATGROUP_Bieda);

UBattleDrummerProcessor::UBattleDrummerProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;

	// Run after the state machine settles; ordering relative to the officer is
	// irrelevant since drummers only chase their own slot.
	ExecutionOrder.ExecuteAfter.Add(FName(TEXT("BattleStateProcessor")));
}

void UBattleDrummerProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	DrummerQuery.Initialize(EntityManager);
	DrummerQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadWrite);
	DrummerQuery.AddRequirement<FDrummerFragment>(EMassFragmentAccess::ReadWrite);
	DrummerQuery.RegisterWithProcessor(*this);
}

void UBattleDrummerProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	if (BattleSimPaused()) return;
	SCOPE_CYCLE_COUNTER(STAT_BiedaDrummer);
	const float DT = Context.GetDeltaTimeSeconds() * BattleSimTimeScale();

	DrummerQuery.ForEachEntityChunk(Context, [DT](FMassExecutionContext& Ctx)
	{
		auto Transforms = Ctx.GetMutableFragmentView<FTransformFragment>();
		auto Drummers   = Ctx.GetMutableFragmentView<FDrummerFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			FDrummerFragment& DR = Drummers[i];
			if (!DR.bIsAlive || !DR.bHasFormationPos) continue;

			const FVector Pos      = Transforms[i].GetTransform().GetLocation();
			const FVector ToTarget = DR.FormationPos - Pos;
			const float   DistSq   = ToTarget.SizeSquared2D();

			// Slot threshold matches the officer's (120 cm) — close enough = stop.
			if (DistSq > FMath::Square(120.f))
			{
				const FVector Dir = ToTarget.GetSafeNormal2D();
				FTransform T = Transforms[i].GetTransform();
				T.SetLocation(Pos + Dir * DR.MoveSpeed * DT);
				T.SetRotation(FRotationMatrix::MakeFromX(Dir).ToQuat());
				Transforms[i].GetMutableTransform() = T;
			}
		}
	});
}
