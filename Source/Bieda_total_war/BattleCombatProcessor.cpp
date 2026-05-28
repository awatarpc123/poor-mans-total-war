#include "BattleCombatProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleSpatialGrid.h"
#include "DrawDebugHelpers.h"

namespace
{
	struct FCombatSnap
	{
		uint8       TeamId;
		EAgentState State;
	};

	struct FBattleShotEvent
	{
		FMassEntityHandle Target;
		FVector ShooterPos;
		FVector TargetPos;
		bool    bHit;
	};
}

UBattleCombatProcessor::UBattleCombatProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;
	bRequiresGameThreadExecution = true;   // DrawDebugLine + direct EntityManager access

	ExecutionOrder.ExecuteAfter.Add(FName(TEXT("BattleStateProcessor")));
	ExecutionOrder.ExecuteBefore.Add(FName(TEXT("BattleOfficerProcessor")));
}

void UBattleCombatProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	CombatQuery.Initialize(EntityManager);
	CombatQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadWrite);
	CombatQuery.AddRequirement<FAgentStateFragment>(EMassFragmentAccess::ReadOnly);
	CombatQuery.AddRequirement<FAgentCombatFragment>(EMassFragmentAccess::ReadWrite);
	CombatQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	CombatQuery.RegisterWithProcessor(*this);
}

void UBattleCombatProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	UWorld* World = GetWorld();

	// -------------------------------------------------------------------
	// Pass 0: snapshot positions, teams, states + entity handles
	// -------------------------------------------------------------------
	TArray<FCombatSnap>       Snaps;
	TArray<FVector>           Positions;
	TArray<FVector>           Forwards;
	TArray<FMassEntityHandle> Handles;

	CombatQuery.ForEachEntityChunk(Context,
		[&Snaps, &Positions, &Forwards, &Handles](FMassExecutionContext& Ctx)
	{
		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		const auto States     = Ctx.GetFragmentView<FAgentStateFragment>();
		const auto Factions   = Ctx.GetFragmentView<FFactionFragment>();
		const int32 N         = Ctx.GetNumEntities();

		Snaps.Reserve(Snaps.Num() + N);
		Positions.Reserve(Positions.Num() + N);
		Forwards.Reserve(Forwards.Num() + N);
		Handles.Reserve(Handles.Num() + N);

		for (int32 i = 0; i < N; ++i)
		{
			const FTransform& T = Transforms[i].GetTransform();
			Positions.Add(T.GetLocation());
			Forwards.Add(T.GetUnitAxis(EAxis::X));
			Snaps.Add({ Factions[i].TeamId, States[i].State });
			Handles.Add(Ctx.GetEntity(i));
		}
	});

	if (Snaps.IsEmpty()) return;

	// Spatial grid — cell size ~half fire range for efficient lookups
	FBattleSpatialGrid Grid;
	Grid.Build(Positions, 2500.f);

	// -------------------------------------------------------------------
	// Pass 1: target acquisition + damage collection
	// -------------------------------------------------------------------
	TArray<FBattleShotEvent> DamageEvents;
	int32 GlobalIdx = 0;

	CombatQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& Ctx)
	{
		auto       Transforms = Ctx.GetMutableFragmentView<FTransformFragment>();
		const auto States     = Ctx.GetFragmentView<FAgentStateFragment>();
		auto       Combats    = Ctx.GetMutableFragmentView<FAgentCombatFragment>();
		const auto Factions   = Ctx.GetFragmentView<FFactionFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i, ++GlobalIdx)
		{
			const EAgentState MyState = States[i].State;
			FAgentCombatFragment& CF  = Combats[i];
			const uint8 MyTeam        = Factions[i].TeamId;
			const FVector MyPos       = Positions[GlobalIdx];
			const FVector MyForward   = Forwards[GlobalIdx];

			// ── AIMING: acquire / validate target ──────────────────────────
			if (MyState == EAgentState::AIMING)
			{
				// — Find target index in snapshot (for validation & position) —
				int32 TargetSnapIdx = INDEX_NONE;
				if (CF.bHasAcquiredTarget)
				{
					for (int32 h = 0; h < Handles.Num(); ++h)
					{
						if (Handles[h] == CF.TargetEntity)
						{
							TargetSnapIdx = h;
							break;
						}
					}
				}

				// — Validate existing target ─────────────────────────────────
				if (CF.bHasAcquiredTarget)
				{
					bool bStillValid = false;
					if (TargetSnapIdx != INDEX_NONE &&
						Snaps[TargetSnapIdx].State != EAgentState::DEAD)
					{
						const float DistSq = (Positions[TargetSnapIdx] - MyPos).SizeSquared2D();
						if (DistSq < FMath::Square(CF.FireRange))
							bStillValid = true;
					}
					if (!bStillValid)
					{
						CF.bHasAcquiredTarget = false;
						TargetSnapIdx = INDEX_NONE;
					}
				}

				// — Search for new target ────────────────────────────────────
				if (!CF.bHasAcquiredTarget)
				{
					float BestDistSq = FMath::Square(CF.FireRange);
					int32 BestIdx    = INDEX_NONE;

					const float CosHalf = FMath::Cos(
						FMath::DegreesToRadians(CF.VisionHalfAngleDeg));

					Grid.ForEachInRadius(MyPos, CF.FireRange, GlobalIdx,
						[&](int32 j, float DistSq2D)
					{
						if (Snaps[j].TeamId == MyTeam) return;           // same team
						if (Snaps[j].State == EAgentState::DEAD) return; // dead

						// ── Vision cone check ───────────────────────────
						const FVector ToTarget =
							(Positions[j] - MyPos).GetSafeNormal2D();
						const float Dot = FVector::DotProduct(
							MyForward.GetSafeNormal2D(), ToTarget);
						if (Dot < CosHalf) return; // outside vision cone

						if (DistSq2D < BestDistSq)
						{
							BestDistSq = DistSq2D;
							BestIdx    = j;
						}
					});

					if (BestIdx != INDEX_NONE)
					{
						CF.TargetEntity       = Handles[BestIdx];
						CF.bHasAcquiredTarget = true;
						TargetSnapIdx         = BestIdx;
					}
				}

				// — Face the target ──────────────────────────────────────────
				if (CF.bHasAcquiredTarget && TargetSnapIdx != INDEX_NONE)
				{
					const FVector ToTarget =
						(Positions[TargetSnapIdx] - MyPos).GetSafeNormal2D();
					if (!ToTarget.IsNearlyZero())
					{
						FTransform T = Transforms[i].GetTransform();
						T.SetRotation(
							FRotationMatrix::MakeFromX(ToTarget).ToQuat());
						Transforms[i].GetMutableTransform() = T;
					}

					// Thin orange aiming line (single-frame)
					if (World)
					{
						DrawDebugLine(World,
							MyPos + FVector(0.f, 0.f, 75.f),
							Positions[TargetSnapIdx] + FVector(0.f, 0.f, 75.f),
							FColor::Orange, false, -1.f, 0, 1.f);
					}
				}
			}
			// ── FIRING: roll accuracy, queue damage ────────────────────────
			else if (MyState == EAgentState::FIRING && CF.bHasAcquiredTarget)
			{
				// Find target position for debug line
				FVector TargetPos = MyPos + MyForward * CF.FireRange;
				for (int32 h = 0; h < Handles.Num(); ++h)
				{
					if (Handles[h] == CF.TargetEntity)
					{
						TargetPos = Positions[h];
						break;
					}
				}

				const bool bHit = FMath::FRand() < CF.Accuracy;
				DamageEvents.Add({ CF.TargetEntity, MyPos, TargetPos, bHit });

				CF.bHasAcquiredTarget = false;   // shot fired — clear target
			}
			// ── Other states: clear stale target ───────────────────────────
			else
			{
				CF.bHasAcquiredTarget = false;
			}
		}
	});

	// -------------------------------------------------------------------
	// Pass 2: apply damage + debug shot lines
	// -------------------------------------------------------------------
	for (const FBattleShotEvent& Evt : DamageEvents)
	{
		if (Evt.bHit && EntityManager.IsEntityValid(Evt.Target))
		{
			FAgentCombatFragment& TargetCF =
				EntityManager.GetFragmentDataChecked<FAgentCombatFragment>(Evt.Target);

			if (TargetCF.HP <= 0.f) continue;   // already dead (killed earlier this frame)

			TargetCF.HP -= 100.f;

			if (TargetCF.HP <= 0.f)
			{
				FAgentStateFragment& TargetSF =
					EntityManager.GetFragmentDataChecked<FAgentStateFragment>(Evt.Target);
				TargetSF.State      = EAgentState::DEAD;
				TargetSF.StateTimer = 0.f;
			}
		}

		// Debug line: red = hit, yellow = miss.  Persists 0.3 s.
		if (World)
		{
			const FVector Start = Evt.ShooterPos + FVector(0.f, 0.f, 75.f);
			const FVector End   = Evt.TargetPos  + FVector(0.f, 0.f, 75.f);
			const FColor  Color = Evt.bHit ? FColor::Red : FColor(255, 200, 0);
			const float   Thick = Evt.bHit ? 2.5f : 1.f;
			DrawDebugLine(World, Start, End, Color, false, 0.3f, 0, Thick);
		}
	}
}
