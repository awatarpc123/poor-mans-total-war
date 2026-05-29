#include "BattleOrderProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleSpatialGrid.h"

namespace
{
	struct FOrderOfficerSnap
	{
		FVector Position;
		bool    bIsAlive;
		uint8   SquadId;
	};

	struct FDrummerSnap
	{
		FVector Position;
		bool    bIsAlive;
		uint8   SquadId;
		float   DrumRadius;
	};

	struct FOrderSoldierSnap
	{
		bool  bExecuted;
		bool  bRouting;
		bool  bDead;
		uint8 SquadId;
	};

	constexpr float VisualRadius    = 500.f;
	constexpr float VisualThreshold = 0.6f;
}

UBattleOrderProcessor::UBattleOrderProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;
	ExecutionOrder.ExecuteBefore.Add(FName(TEXT("BattleStateProcessor")));
}

void UBattleOrderProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	OfficerQuery.Initialize(EntityManager);
	OfficerQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	OfficerQuery.AddRequirement<FOfficerFragment>(EMassFragmentAccess::ReadOnly);
	OfficerQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	OfficerQuery.RegisterWithProcessor(*this);

	DrummerQuery.Initialize(EntityManager);
	DrummerQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	DrummerQuery.AddRequirement<FDrummerFragment>(EMassFragmentAccess::ReadOnly);
	DrummerQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	DrummerQuery.RegisterWithProcessor(*this);

	SoldierQuery.Initialize(EntityManager);
	SoldierQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	SoldierQuery.AddRequirement<FAgentStateFragment>(EMassFragmentAccess::ReadWrite);
	SoldierQuery.AddRequirement<FMoraleFragment>(EMassFragmentAccess::ReadOnly);
	SoldierQuery.AddRequirement<FOrderPropagationFragment>(EMassFragmentAccess::ReadWrite);
	SoldierQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	SoldierQuery.RegisterWithProcessor(*this);
}

void UBattleOrderProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	const float DT = Context.GetDeltaTimeSeconds();

	// -----------------------------------------------------------------------
	// Pass 0: snapshot officers
	// -----------------------------------------------------------------------
	TArray<FOrderOfficerSnap> Officers;
	OfficerQuery.ForEachEntityChunk(Context, [&Officers](FMassExecutionContext& Ctx)
	{
		const auto Transforms  = Ctx.GetFragmentView<FTransformFragment>();
		const auto OfficerData = Ctx.GetFragmentView<FOfficerFragment>();
		const auto Factions    = Ctx.GetFragmentView<FFactionFragment>();
		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
			Officers.Add({ Transforms[i].GetTransform().GetLocation(), OfficerData[i].bIsAlive, Factions[i].SquadId });
	});

	// -----------------------------------------------------------------------
	// Pass 0b: snapshot drummers (order relay nodes)
	// -----------------------------------------------------------------------
	TArray<FDrummerSnap> Drummers;
	DrummerQuery.ForEachEntityChunk(Context, [&Drummers](FMassExecutionContext& Ctx)
	{
		const auto Transforms  = Ctx.GetFragmentView<FTransformFragment>();
		const auto DrummerData = Ctx.GetFragmentView<FDrummerFragment>();
		const auto Factions    = Ctx.GetFragmentView<FFactionFragment>();
		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
			Drummers.Add({ Transforms[i].GetTransform().GetLocation(),
				DrummerData[i].bIsAlive, Factions[i].SquadId, DrummerData[i].DrumRadius });
	});

	// -----------------------------------------------------------------------
	// Pass 1: snapshot all soldiers + positions for spatial grid
	// -----------------------------------------------------------------------
	TArray<FOrderSoldierSnap> AllSoldiers;
	TArray<FVector>      Positions;

	SoldierQuery.ForEachEntityChunk(Context, [&AllSoldiers, &Positions](FMassExecutionContext& Ctx)
	{
		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		const auto States     = Ctx.GetFragmentView<FAgentStateFragment>();
		const auto PropData   = Ctx.GetFragmentView<FOrderPropagationFragment>();
		const auto Factions   = Ctx.GetFragmentView<FFactionFragment>();
		const int32 N = Ctx.GetNumEntities();
		AllSoldiers.Reserve(AllSoldiers.Num() + N);
		Positions.Reserve(Positions.Num() + N);
		for (int32 i = 0; i < N; ++i)
		{
			const EAgentState S = States[i].State;
			Positions.Add(Transforms[i].GetTransform().GetLocation());
			AllSoldiers.Add({
				PropData[i].bOrderExecuted,
				S == EAgentState::ROUTING,
				S == EAgentState::DEAD,
				Factions[i].SquadId
			});
		}
	});

	FBattleSpatialGrid Grid;
	Grid.Build(Positions, VisualRadius);

	// -----------------------------------------------------------------------
	// Pass 2: update each soldier
	// -----------------------------------------------------------------------
	int32 GlobalIdx = 0;
	SoldierQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& Ctx)
	{
		auto       States   = Ctx.GetMutableFragmentView<FAgentStateFragment>();
		const auto Morales  = Ctx.GetFragmentView<FMoraleFragment>();
		auto       PropData = Ctx.GetMutableFragmentView<FOrderPropagationFragment>();
		const auto Factions = Ctx.GetFragmentView<FFactionFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i, ++GlobalIdx)
		{
			FOrderPropagationFragment& Prop = PropData[i];
			const FVector MyPos   = Positions[GlobalIdx];
			const uint8  MySquad = Factions[i].SquadId;

			if (Prop.bOrderExecuted) continue;

			// ── Ignored: visual check (same squad) ────────────────────────
			if (Prop.bOrderIgnored)
			{
				int32 NearbyTotal    = 0;
				int32 NearbyExecuted = 0;

				Grid.ForEachInRadius(MyPos, VisualRadius, GlobalIdx,
					[&](int32 j, float /*DistSq2D*/)
					{
						if (AllSoldiers[j].bDead) return;
						if (AllSoldiers[j].SquadId != MySquad) return;
						++NearbyTotal;
						if (AllSoldiers[j].bExecuted) ++NearbyExecuted;
					});

				if (NearbyTotal > 0 &&
					NearbyExecuted >= FMath::CeilToInt(NearbyTotal * VisualThreshold))
				{
					Prop.bOrderIgnored  = false;
					Prop.bOrderReceived = true;
					Prop.ExecutionDelay = FMath::RandRange(0.8f, 2.0f);

					const EAgentState S = States[i].State;
					if (S == EAgentState::LOADING || S == EAgentState::AIMING ||
						S == EAgentState::FIRING)
					{
						States[i].State      = EAgentState::HOLDING;
						States[i].StateTimer = 0.f;
					}
				}
				continue;
			}

			// ── Order received: tick delay ────────────────────────────────
			if (Prop.bOrderReceived)
			{
				Prop.ExecutionTimer += DT;
				if (Prop.ExecutionTimer >= Prop.ExecutionDelay)
				{
					const EAgentState S = States[i].State;
					if (S == EAgentState::HOLDING || S == EAgentState::LOADING ||
						S == EAgentState::AIMING  || S == EAgentState::FIRING)
					{
						States[i].State      = EAgentState::ADVANCING;
						States[i].StateTimer = 0.f;
						Prop.bOrderExecuted  = true;
					}
				}
				continue;
			}

			// ── Officer voice (own squad only) ───────────────────────────
			bool bGotOrder = false;
			for (const FOrderOfficerSnap& Off : Officers)
			{
				if (!Off.bIsAlive) continue;
				if (Off.SquadId != MySquad) continue;
				if ((Off.Position - MyPos).SizeSquared2D() < FMath::Square(OfficerVoiceRadius))
				{
					Prop.bOrderReceived = true;
					Prop.ExecutionDelay = FMath::RandRange(0.f, 0.3f);
					bGotOrder = true;
					break;
				}
			}

			// ── Drummer relay (own squad only) ───────────────────────────
			// A living drummer extends the officer's reach: soldiers inside the
			// drumbeat radius hear the order with a small relay delay.
			if (!bGotOrder)
			{
				for (const FDrummerSnap& Drm : Drummers)
				{
					if (!Drm.bIsAlive) continue;
					if (Drm.SquadId != MySquad) continue;
					if ((Drm.Position - MyPos).SizeSquared2D() < FMath::Square(Drm.DrumRadius))
					{
						Prop.bOrderReceived = true;
						Prop.ExecutionDelay = FMath::RandRange(0.1f, 0.4f);
						bGotOrder = true;
						break;
					}
				}
			}

			// ── Peer propagation (own squad only) ────────────────────────
			if (!bGotOrder)
			{
				bool bFound = false;
				Grid.ForEachInRadius(MyPos, PropagationRadius, GlobalIdx,
					[&](int32 j, float /*DistSq2D*/)
					{
						if (bFound) return;
						if (!AllSoldiers[j].bExecuted) return;
						if (AllSoldiers[j].SquadId != MySquad) return;

						const float NoiseChance = (Morales[i].Morale < 50.f) ? 0.10f : 0.03f;
						if (FMath::FRand() < NoiseChance)
							Prop.bOrderIgnored = true;
						else
						{
							Prop.bOrderReceived = true;
							Prop.ExecutionDelay = FMath::RandRange(0.3f, 0.8f);
						}
						bFound = true;
					});
			}
		}
	});
}
