#include "BattleMoraleProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleThreatActor.h"
#include "EngineUtils.h"
#include "BattleSpatialGrid.h"

namespace
{
	struct FAgentSnap
	{
		EAgentState State;
		uint8       TeamId;
	};

	struct FOfficerSnap
	{
		FVector Position;
		float   MoraleRadius;
		float   MoraleBonus;
		bool    bIsAlive;
		bool    bJustDied;
		uint8   SquadId;
	};

	struct FThreatSnap
	{
		FVector Position;
		float   Radius;
		float   DrainRate;
	};
}

UBattleMoraleProcessor::UBattleMoraleProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;
	bRequiresGameThreadExecution = true;

	ExecutionOrder.ExecuteAfter.Add(FName(TEXT("BattleOfficerProcessor")));
	ExecutionOrder.ExecuteBefore.Add(FName(TEXT("BattleMovementProcessor")));
}

void UBattleMoraleProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	MoraleQuery.Initialize(EntityManager);
	MoraleQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	MoraleQuery.AddRequirement<FAgentStateFragment>(EMassFragmentAccess::ReadOnly);
	MoraleQuery.AddRequirement<FMoraleFragment>(EMassFragmentAccess::ReadWrite);
	MoraleQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	MoraleQuery.RegisterWithProcessor(*this);

	OfficerQuery.Initialize(EntityManager);
	OfficerQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	OfficerQuery.AddRequirement<FOfficerFragment>(EMassFragmentAccess::ReadOnly);
	OfficerQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	OfficerQuery.RegisterWithProcessor(*this);
}

void UBattleMoraleProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	// -----------------------------------------------------------------------
	// Pass 0a: threat zones
	// -----------------------------------------------------------------------
	TArray<FThreatSnap> Threats;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABattleThreatActor> It(World); It; ++It)
			Threats.Add({ It->GetActorLocation(), It->ThreatRadius, It->MoraleDrainRate });
	}

	// -----------------------------------------------------------------------
	// Pass 0b: officer snapshots (SquadId for binding to specific soldiers)
	// -----------------------------------------------------------------------
	TArray<FOfficerSnap> Officers;

	OfficerQuery.ForEachEntityChunk(Context, [&Officers](FMassExecutionContext& Ctx)
	{
		const auto Transforms  = Ctx.GetFragmentView<FTransformFragment>();
		const auto OfficerData = Ctx.GetFragmentView<FOfficerFragment>();
		const auto Factions    = Ctx.GetFragmentView<FFactionFragment>();
		const int32 N = Ctx.GetNumEntities();
		Officers.Reserve(Officers.Num() + N);
		for (int32 i = 0; i < N; ++i)
		{
			Officers.Add({
				Transforms[i].GetTransform().GetLocation(),
				OfficerData[i].MoraleRadius,
				OfficerData[i].MoraleBonus,
				OfficerData[i].bIsAlive,
				OfficerData[i].bJustDied,
				Factions[i].SquadId
			});
		}
	});

	// -----------------------------------------------------------------------
	// Pass 1: soldier snapshots + positions
	// -----------------------------------------------------------------------
	TArray<FAgentSnap> Snaps;
	TArray<FVector>    Positions;

	MoraleQuery.ForEachEntityChunk(Context, [&Snaps, &Positions](FMassExecutionContext& Ctx)
	{
		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		const auto States     = Ctx.GetFragmentView<FAgentStateFragment>();
		const auto Factions   = Ctx.GetFragmentView<FFactionFragment>();
		const int32 N         = Ctx.GetNumEntities();
		Snaps.Reserve(Snaps.Num() + N);
		Positions.Reserve(Positions.Num() + N);
		for (int32 i = 0; i < N; ++i)
		{
			Positions.Add(Transforms[i].GetTransform().GetLocation());
			Snaps.Add({ States[i].State, Factions[i].TeamId });
		}
	});

	if (Snaps.IsEmpty()) return;

	FBattleSpatialGrid Grid;
	Grid.Build(Positions, RoutingRadius);

	const float DT = Context.GetDeltaTimeSeconds();

	// -----------------------------------------------------------------------
	// Pass 2: apply morale effects
	// -----------------------------------------------------------------------
	int32 GlobalIdx = 0;

	MoraleQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& Ctx)
	{
		const auto States   = Ctx.GetFragmentView<FAgentStateFragment>();
		auto       Morales  = Ctx.GetMutableFragmentView<FMoraleFragment>();
		const auto Factions = Ctx.GetFragmentView<FFactionFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i, ++GlobalIdx)
		{
			const EAgentState MyState  = States[i].State;
			const uint8       MyTeam   = Factions[i].TeamId;
			const uint8       MySquad  = Factions[i].SquadId;
			float& Morale = Morales[i].Morale;
			const FVector MyPos = Positions[GlobalIdx];

			if (MyState == EAgentState::DEAD) continue;

			// ── Threat zones ──────────────────────────────────────────────
			for (const FThreatSnap& Threat : Threats)
			{
				if ((Threat.Position - MyPos).SizeSquared2D() < FMath::Square(Threat.Radius))
					Morale -= Threat.DrainRate * DT;
			}

			if (MyState == EAgentState::ROUTING ||
				MyState == EAgentState::RALLYING)
			{
				Morale = FMath::Clamp(Morale, 0.f, 100.f);
				continue;
			}

			// ── Peer effects — by TeamId (cross-squad) ────────────────────
			// Routing/dead allies from ANY squad of same team affect morale.
			// Stable allies from same team also provide bonus.
			float RoutingDrain = 0.f;
			float DeadDrain    = 0.f;
			float StableBonus  = 0.f;

			Grid.ForEachInRadius(MyPos, RoutingRadius, GlobalIdx,
				[&](int32 j, float DistSq2D)
				{
					if (Snaps[j].TeamId != MyTeam) return;

					switch (Snaps[j].State)
					{
					case EAgentState::ROUTING:
						RoutingDrain += RoutingDrainRate;
						break;
					case EAgentState::DEAD:
						if (DistSq2D < FMath::Square(DeadRadius))
							DeadDrain += DeadDrainRate;
						break;
					default:
						if (DistSq2D < FMath::Square(StableRadius))
							StableBonus += StableBonusRate;
						break;
					}
				});

			RoutingDrain = FMath::Min(RoutingDrain, MaxRoutingDrain);
			DeadDrain    = FMath::Min(DeadDrain,    MaxDeadDrain);
			StableBonus  = FMath::Min(StableBonus,  MaxStableBonus);

			Morale -= (RoutingDrain + DeadDrain) * DT;
			Morale += StableBonus * DT;

			// ── Officer effects — by SquadId (own officer only) ───────────
			for (const FOfficerSnap& Off : Officers)
			{
				if (Off.SquadId != MySquad) continue;

				const float DistSq = (Off.Position - MyPos).SizeSquared2D();

				if (Off.bIsAlive && DistSq < FMath::Square(Off.MoraleRadius))
					Morale += Off.MoraleBonus * DT;

				if (Off.bJustDied && DistSq < FMath::Square(2000.f))
					Morale -= 30.f;
			}

			Morale = FMath::Clamp(Morale, 0.f, 100.f);
		}
	});
}
