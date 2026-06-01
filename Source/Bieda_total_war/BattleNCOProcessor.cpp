#include "BattleNCOProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleStats.h"

DECLARE_CYCLE_STAT(TEXT("NCO"), STAT_BiedaNCO, STATGROUP_Bieda);

namespace
{
	struct FNCOSoldierSnap
	{
		FVector           Position;
		EAgentState       State;
		uint8             SquadId;
		bool              bOrderReceived;
		bool              bOrderExecuted;
		bool              bOrderIgnored;
		FMassEntityHandle Handle;
	};
}

UBattleNCOProcessor::UBattleNCOProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;
	bRequiresGameThreadExecution = true;   // direct EntityManager access for order delivery

	ExecutionOrder.ExecuteAfter.Add(FName(TEXT("BattleOrderProcessor")));
	ExecutionOrder.ExecuteBefore.Add(FName(TEXT("BattleStateProcessor")));
}

void UBattleNCOProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	NCOQuery.Initialize(EntityManager);
	NCOQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadWrite);
	NCOQuery.AddRequirement<FNCOFragment>(EMassFragmentAccess::ReadWrite);
	NCOQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	NCOQuery.AddRequirement<FMoraleFragment>(EMassFragmentAccess::ReadOnly);
	NCOQuery.RegisterWithProcessor(*this);

	SoldierSnapshotQuery.Initialize(EntityManager);
	SoldierSnapshotQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	SoldierSnapshotQuery.AddRequirement<FAgentStateFragment>(EMassFragmentAccess::ReadOnly);
	SoldierSnapshotQuery.AddRequirement<FOrderPropagationFragment>(EMassFragmentAccess::ReadOnly);
	SoldierSnapshotQuery.AddRequirement<FFactionFragment>(EMassFragmentAccess::ReadOnly);
	SoldierSnapshotQuery.RegisterWithProcessor(*this);
}

void UBattleNCOProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	SCOPE_CYCLE_COUNTER(STAT_BiedaNCO);
	const float DT = Context.GetDeltaTimeSeconds();

	// -------------------------------------------------------------------
	// Pass 0: snapshot all soldiers
	// -------------------------------------------------------------------
	TArray<FNCOSoldierSnap> Soldiers;

	SoldierSnapshotQuery.ForEachEntityChunk(Context,
		[&Soldiers](FMassExecutionContext& Ctx)
	{
		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		const auto States     = Ctx.GetFragmentView<FAgentStateFragment>();
		const auto PropData   = Ctx.GetFragmentView<FOrderPropagationFragment>();
		const auto Factions   = Ctx.GetFragmentView<FFactionFragment>();
		const int32 N = Ctx.GetNumEntities();
		Soldiers.Reserve(Soldiers.Num() + N);

		for (int32 i = 0; i < N; ++i)
		{
			Soldiers.Add({
				Transforms[i].GetTransform().GetLocation(),
				States[i].State,
				Factions[i].SquadId,
				PropData[i].bOrderReceived,
				PropData[i].bOrderExecuted,
				PropData[i].bOrderIgnored,
				Ctx.GetEntity(i)
			});
		}
	});

	// -------------------------------------------------------------------
	// Pass 1: NCO AI — find target, move, deliver order / rally
	// -------------------------------------------------------------------
	// ClaimedTargets prevents multiple NCOs from dog-piling the same soldier.
	// An NCO that already has a target "claims" it; others will skip him.
	TSet<FMassEntityHandle> ClaimedTargets;

	// Pre-seed with targets NCOs already hold from previous frames.
	NCOQuery.ForEachEntityChunk(Context,
		[&ClaimedTargets](FMassExecutionContext& Ctx)
	{
		const auto NCOs = Ctx.GetFragmentView<FNCOFragment>();
		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			if (NCOs[i].bIsAlive && NCOs[i].bHasTarget)
				ClaimedTargets.Add(NCOs[i].TargetSoldier);
		}
	});

	NCOQuery.ForEachEntityChunk(Context,
		[&Soldiers, &EntityManager, &ClaimedTargets, DT](FMassExecutionContext& Ctx)
	{
		auto       Transforms = Ctx.GetMutableFragmentView<FTransformFragment>();
		auto       NCOs       = Ctx.GetMutableFragmentView<FNCOFragment>();
		const auto Factions   = Ctx.GetFragmentView<FFactionFragment>();
		const auto Morales    = Ctx.GetFragmentView<FMoraleFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
		{
			FNCOFragment& NCO       = NCOs[i];
			const uint8   MySquad   = Factions[i].SquadId;
			const FVector NCOPos    = Transforms[i].GetTransform().GetLocation();

			if (!NCO.bIsAlive) continue;

			// ── Validate current target ───────────────────────────────────
			if (NCO.bHasTarget)
			{
				bool bStillValid = false;
				for (const FNCOSoldierSnap& S : Soldiers)
				{
					if (S.Handle != NCO.TargetSoldier) continue;
					if (S.SquadId != MySquad) break;
					if (S.State == EAgentState::DEAD) break;

					// Straggler: still hasn't received/executed order
					if (!S.bOrderExecuted && (S.bOrderIgnored || !S.bOrderReceived))
					{
						bStillValid = true;
						break;
					}
					// Routing/shaken: still needs steadying
					if (S.State == EAgentState::ROUTING ||
						S.State == EAgentState::SHAKEN)
					{
						bStillValid = true;
						break;
					}
					break;
				}

				if (!bStillValid)
				{
					NCO.bHasTarget = false;
				}
			}

			// ── Find new target ───────────────────────────────────────────
			if (!NCO.bHasTarget)
			{
				float BestDistSq = FLT_MAX;
				int32 BestIdx    = INDEX_NONE;

				// Priority 1: wavering (SHAKEN) or routing soldiers — steady/rally.
				// SHAKEN included so NCOs run up BEFORE the man fully breaks.
				// Skip soldiers another NCO is already chasing (ClaimedTargets).
				for (int32 s = 0; s < Soldiers.Num(); ++s)
				{
					const FNCOSoldierSnap& S = Soldiers[s];
					if (S.SquadId != MySquad) continue;
					if (S.State != EAgentState::ROUTING &&
						S.State != EAgentState::SHAKEN) continue;
					if (ClaimedTargets.Contains(S.Handle)) continue;

					const float DistSq = (S.Position - NCOPos).SizeSquared2D();
					if (DistSq < BestDistSq)
					{
						BestDistSq = DistSq;
						BestIdx    = s;
					}
				}

				// Priority 2: stragglers (didn't hear order)
				if (BestIdx == INDEX_NONE)
				{
					for (int32 s = 0; s < Soldiers.Num(); ++s)
					{
						const FNCOSoldierSnap& S = Soldiers[s];
						if (S.SquadId != MySquad) continue;
						if (S.State == EAgentState::DEAD) continue;
						if (S.bOrderExecuted) continue;
						if (!S.bOrderIgnored && S.bOrderReceived) continue;
						if (ClaimedTargets.Contains(S.Handle)) continue;

						const float DistSq = (S.Position - NCOPos).SizeSquared2D();
						if (DistSq < BestDistSq)
						{
							BestDistSq = DistSq;
							BestIdx    = s;
						}
					}
				}

				if (BestIdx != INDEX_NONE)
				{
					NCO.TargetSoldier = Soldiers[BestIdx].Handle;
					NCO.bHasTarget    = true;
					ClaimedTargets.Add(Soldiers[BestIdx].Handle);
				}
			}

			// ── Determine move target ─────────────────────────────────────
			FVector MoveTarget = NCOPos;
			bool    bShouldMove = false;

			if (NCO.bHasTarget)
			{
				// Move toward target soldier
				for (const FNCOSoldierSnap& S : Soldiers)
				{
					if (S.Handle == NCO.TargetSoldier)
					{
						MoveTarget  = S.Position;
						bShouldMove = true;
						break;
					}
				}
			}
			else if (NCO.bHasFormationPos)
			{
				// Return to formation rear position
				MoveTarget  = NCO.FormationPos;
				bShouldMove = true;
			}

			// ── Movement ──────────────────────────────────────────────────
			if (bShouldMove)
			{
				const FVector ToTarget = MoveTarget - NCOPos;
				const float   DistSq   = ToTarget.SizeSquared2D();

				if (DistSq > FMath::Square(80.f))
				{
					// ChaseSpeed when actively chasing someone (straggler/routing),
					// MoveSpeed (formation pace) when returning to FormationPos.
					const float Speed = NCO.bHasTarget ? NCO.ChaseSpeed : NCO.MoveSpeed;

					const FVector Dir = ToTarget.GetSafeNormal2D();
					FTransform T = Transforms[i].GetTransform();
					T.SetLocation(NCOPos + Dir * Speed * DT);
					T.SetRotation(FRotationMatrix::MakeFromX(Dir).ToQuat());
					Transforms[i].GetMutableTransform() = T;
				}
			}

			// ── Actions when close to target ──────────────────────────────
			if (NCO.bHasTarget)
			{
				FVector TargetPos = NCOPos;
				EAgentState TargetState = EAgentState::DEAD;

				for (const FNCOSoldierSnap& S : Soldiers)
				{
					if (S.Handle == NCO.TargetSoldier)
					{
						TargetPos   = S.Position;
						TargetState = S.State;
						break;
					}
				}

				const float DistToTarget = (TargetPos - NCOPos).Size2D();

				// Close enough to interact
				if (DistToTarget < 200.f)
				{
					if (TargetState == EAgentState::ROUTING ||
						TargetState == EAgentState::SHAKEN)
					{
						// Rally: boost morale of routing/shaken soldier (continuous).
						// NCO at his side steadies him faster than passive recovery.
						if (EntityManager.IsEntityValid(NCO.TargetSoldier))
						{
							FMoraleFragment& TargetMF =
								EntityManager.GetFragmentDataChecked<FMoraleFragment>(NCO.TargetSoldier);
							TargetMF.Morale = FMath::Min(100.f,
								TargetMF.Morale + NCO.RallyMoraleBoost * DT);
						}
						// Stay with him until he steadies (don't clear target)
					}
					else
					{
						// Straggler: force-deliver the order
						if (EntityManager.IsEntityValid(NCO.TargetSoldier))
						{
							FOrderPropagationFragment& Prop =
								EntityManager.GetFragmentDataChecked<FOrderPropagationFragment>(
									NCO.TargetSoldier);
							Prop.bOrderIgnored  = false;
							Prop.bOrderReceived = true;
							Prop.ExecutionDelay = FMath::RandRange(0.2f, 0.5f);
							Prop.ExecutionTimer = 0.f;
						}
						// Done with this soldier — move on
						NCO.bHasTarget = false;
					}
				}
			}

			// ── Rally aura: boost morale of ALL nearby routing squadmates ─
			// (Applied unconditionally — the aura helps even while the NCO is
			// busy escorting a specific target.)
			{
				for (const FNCOSoldierSnap& S : Soldiers)
				{
					if (S.SquadId != MySquad) continue;
					if (S.State != EAgentState::ROUTING &&
						S.State != EAgentState::SHAKEN) continue;

					const float DistSq = (S.Position - NCOPos).SizeSquared2D();
					if (DistSq < FMath::Square(NCO.RallyRadius))
					{
						if (EntityManager.IsEntityValid(S.Handle))
						{
							FMoraleFragment& SMF =
								EntityManager.GetFragmentDataChecked<FMoraleFragment>(S.Handle);
							SMF.Morale = FMath::Min(100.f,
								SMF.Morale + NCO.RallyMoraleBoost * 0.3f * DT);
						}
					}
				}
			}
		}
	});
}
