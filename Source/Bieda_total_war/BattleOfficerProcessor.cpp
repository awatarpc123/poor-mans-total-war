#include "BattleOfficerProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"

namespace
{
	struct FSoldierSnap
	{
		FVector     Position;
		EAgentState State;
		uint8       SquadId;
	};
}

UBattleOfficerProcessor::UBattleOfficerProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;

	ExecutionOrder.ExecuteAfter.Add(FName(TEXT("BattleStateProcessor")));
	ExecutionOrder.ExecuteBefore.Add(FName(TEXT("BattleMoraleProcessor")));
}

void UBattleOfficerProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	OfficerQuery.Initialize(EntityManager);
	OfficerQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadWrite);
	OfficerQuery.AddRequirement<FMoraleFragment>(EMassFragmentAccess::ReadWrite);
	OfficerQuery.AddRequirement<FOfficerFragment>(EMassFragmentAccess::ReadWrite);
	OfficerQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	OfficerQuery.RegisterWithProcessor(*this);

	SoldierQuery.Initialize(EntityManager);
	SoldierQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	SoldierQuery.AddRequirement<FAgentStateFragment>(EMassFragmentAccess::ReadOnly);
	SoldierQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	SoldierQuery.RegisterWithProcessor(*this);
}

void UBattleOfficerProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	// -----------------------------------------------------------------------
	// Pass 1: snapshot all soldiers (position + state + squad)
	// -----------------------------------------------------------------------
	TArray<FSoldierSnap> Soldiers;

	SoldierQuery.ForEachEntityChunk(Context, [&Soldiers](FMassExecutionContext& Ctx)
	{
		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		const auto States     = Ctx.GetFragmentView<FAgentStateFragment>();
		const auto Factions   = Ctx.GetFragmentView<FFactionFragment>();
		const int32 N = Ctx.GetNumEntities();
		Soldiers.Reserve(Soldiers.Num() + N);
		for (int32 i = 0; i < N; ++i)
			Soldiers.Add({ Transforms[i].GetTransform().GetLocation(), States[i].State, Factions[i].SquadId });
	});

	const float DT = Context.GetDeltaTimeSeconds();

	// -----------------------------------------------------------------------
	// Pass 2: update each officer (only considers own-squad soldiers)
	// -----------------------------------------------------------------------
	OfficerQuery.ForEachEntityChunk(Context, [&Soldiers, DT](FMassExecutionContext& Ctx)
	{
		auto Transforms = Ctx.GetMutableFragmentView<FTransformFragment>();
		auto Morales    = Ctx.GetMutableFragmentView<FMoraleFragment>();
		auto Officers   = Ctx.GetMutableFragmentView<FOfficerFragment>();
		const auto Factions = Ctx.GetFragmentView<FFactionFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			FOfficerFragment& OF = Officers[i];
			FMoraleFragment&  MF = Morales[i];
			const uint8 MySquad  = Factions[i].SquadId;

			OF.bJustDied = false;

			if (!OF.bIsAlive) continue;

			const FVector OfficerPos = Transforms[i].GetTransform().GetLocation();

			// -- Analyse own-squad soldiers only --------------------------
			FVector FormationCenter    = FVector::ZeroVector;
			int32   LivingCount        = 0;
			FVector NearestRoutingPos  = FVector::ZeroVector;
			float   NearestRoutingDist = FLT_MAX;
			float   RoutingDrain       = 0.f;

			for (const FSoldierSnap& S : Soldiers)
			{
				if (S.SquadId != MySquad) continue;   // own squad only
				if (S.State == EAgentState::DEAD) continue;

				FormationCenter += S.Position;
				++LivingCount;

				const float DistSq = (S.Position - OfficerPos).SizeSquared();

				if (S.State == EAgentState::ROUTING && DistSq < FMath::Square(3000.f))
				{
					RoutingDrain += 2.f;
					if (DistSq < NearestRoutingDist)
					{
						NearestRoutingDist = DistSq;
						NearestRoutingPos  = S.Position;
					}
				}
			}

			if (LivingCount > 0)
				FormationCenter /= static_cast<float>(LivingCount);

			// -- Morale drain from routing soldiers -----------------------
			RoutingDrain = FMath::Min(RoutingDrain, OF.MoraleDrainCap);
			MF.Morale    = FMath::Max(0.f, MF.Morale - RoutingDrain * DT);

			// -- Death check ----------------------------------------------
			if (MF.Morale <= 0.f)
			{
				OF.bIsAlive  = false;
				OF.bJustDied = true;
				MF.Morale    = 0.f;
				continue;
			}

			// -- Movement -------------------------------------------------
			// Priority: 1) chase routing soldiers  2) formation front  3) centroid
			FVector MoveTarget = FVector::ZeroVector;
			bool    bHasTarget = false;

			if (NearestRoutingDist < FMath::Square(3000.f))
			{
				// Chase nearest routing soldier (existing behavior)
				MoveTarget = NearestRoutingPos;
				bHasTarget = true;
			}
			else if (OF.bHasFormationTarget)
			{
				// Stand at FRONT of formation (set by spawner on move order)
				MoveTarget = OF.FormationFrontPos;
				bHasTarget = true;
			}
			else if (LivingCount > 0)
			{
				// Fallback: follow centroid
				MoveTarget = FormationCenter;
				bHasTarget = true;
			}

			if (bHasTarget)
			{
				const FVector ToTarget = MoveTarget - OfficerPos;
				const float   DistSq   = ToTarget.SizeSquared();

				if (DistSq > FMath::Square(120.f))
				{
					const FVector Dir = ToTarget.GetSafeNormal2D();
					FTransform T = Transforms[i].GetTransform();
					T.SetLocation(OfficerPos + Dir * OF.MoveSpeed * DT);
					T.SetRotation(FRotationMatrix::MakeFromX(Dir).ToQuat());
					Transforms[i].GetMutableTransform() = T;
				}
			}
		}
	});
}
