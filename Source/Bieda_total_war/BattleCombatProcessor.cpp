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

	// Friendly line-of-fire blocking (militia for now): a soldier won't fire if
	// an ally stands in the narrow corridor between him and his target — so a
	// rear-rank man only shoots when the man in front of him is gone (a "wyrwa"
	// gap) or he sees the enemy through a clear lane.
	constexpr float FriendBlockRange     = 1200.f;                          // cm — only nearby ranks block
	constexpr float FriendBlockRangeSq   = FriendBlockRange * FriendBlockRange;
	constexpr float FriendCorridorHalf   = 55.f;                            // cm — body clearance half-width
	constexpr float FriendCorridorHalfSq = FriendCorridorHalf * FriendCorridorHalf;
	constexpr float FriendMinForward     = 30.f;                            // cm — ignore self / abreast
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
	CombatQuery.AddRequirement<FAgentVelocityFragment>(EMassFragmentAccess::ReadOnly);   // UnitType (militia gate)
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
		const auto Velocities = Ctx.GetFragmentView<FAgentVelocityFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i, ++GlobalIdx)
		{
			const EAgentState MyState = States[i].State;
			FAgentCombatFragment& CF  = Combats[i];
			const uint8 MyTeam        = Factions[i].TeamId;
			const FVector MyPos       = Positions[GlobalIdx];
			const FVector MyForward   = Forwards[GlobalIdx];

			// Friendly line-of-fire blocking applies to MILITIA only. Line
			// infantry fights by volley (SquadVolley/RankFire): both ranks fire
			// together on command, so a rear-rank man firing "through" the front
			// rank is the intended thin-red-line behaviour, not friendly fire.
			// The volley coordinator (UpdateVolley) gates their timing instead.
			const bool bUseLaneCheck = (Velocities[i].UnitType == EUnitType::Militia);

			// ── AIMING: acquire / validate target ──────────────────────────
			if (MyState == EAgentState::AIMING)
			{
				const FVector MyForwardN = MyForward.GetSafeNormal2D();
				const float   CosHalf    = FMath::Cos(
					FMath::DegreesToRadians(CF.VisionHalfAngleDeg));

				// — One grid pass: collect enemies (cone+range) and nearby allies —
				TArray<int32, TInlineAllocator<24>> EnemyCand;
				TArray<int32, TInlineAllocator<24>> Friends;   // same team, within block range

				Grid.ForEachInRadius(MyPos, CF.FireRange, GlobalIdx,
					[&](int32 j, float DistSq2D)
				{
					if (Snaps[j].State == EAgentState::DEAD) return;

					if (Snaps[j].TeamId == MyTeam)
					{
						// Gather allies that could sit on a fire lane (militia only).
						if (bUseLaneCheck && DistSq2D < FriendBlockRangeSq)
							Friends.Add(j);
						return;
					}

					// Enemy — vision cone gate.
					const FVector ToTarget = (Positions[j] - MyPos).GetSafeNormal2D();
					if (FVector::DotProduct(MyForwardN, ToTarget) < CosHalf) return;
					EnemyCand.Add(j);
				});

				// — Is the straight line to TgtPos blocked by an ally? ────────
				auto LaneBlocked = [&](const FVector& TgtPos) -> bool
				{
					if (!bUseLaneCheck) return false;
					const float lx = TgtPos.X - MyPos.X;
					const float ly = TgtPos.Y - MyPos.Y;
					const float laneLen = FMath::Sqrt(lx * lx + ly * ly);
					if (laneLen < KINDA_SMALL_NUMBER) return false;
					const float dx = lx / laneLen;
					const float dy = ly / laneLen;
					for (int32 f : Friends)
					{
						const float vx = Positions[f].X - MyPos.X;
						const float vy = Positions[f].Y - MyPos.Y;
						const float fwd = vx * dx + vy * dy;          // along the lane
						if (fwd <= FriendMinForward || fwd >= laneLen) continue;
						const float latSq = (vx * vx + vy * vy) - fwd * fwd; // perp distance²
						if (latSq < FriendCorridorHalfSq) return true;       // ally on the line
					}
					return false;
				};

				// — Locate current target in snapshot —
				int32 TargetSnapIdx = INDEX_NONE;
				if (CF.bHasAcquiredTarget)
				{
					for (int32 h = 0; h < Handles.Num(); ++h)
						if (Handles[h] == CF.TargetEntity) { TargetSnapIdx = h; break; }
				}

				// — Validate existing target: alive, in range, lane clear —
				if (CF.bHasAcquiredTarget)
				{
					bool bStillValid = false;
					if (TargetSnapIdx != INDEX_NONE &&
						Snaps[TargetSnapIdx].State != EAgentState::DEAD)
					{
						const float DistSq = (Positions[TargetSnapIdx] - MyPos).SizeSquared2D();
						if (DistSq < FMath::Square(CF.FireRange) &&
							!LaneBlocked(Positions[TargetSnapIdx]))
							bStillValid = true;
					}
					if (!bStillValid)
					{
						CF.bHasAcquiredTarget = false;
						TargetSnapIdx = INDEX_NONE;
					}
				}

				// — Acquire new target: CLOSEST enemy with a clear fire lane —
				if (!CF.bHasAcquiredTarget)
				{
					float BestDistSq = FMath::Square(CF.FireRange);
					int32 BestIdx    = INDEX_NONE;
					for (int32 j : EnemyCand)
					{
						const float d2 = (Positions[j] - MyPos).SizeSquared2D();
						if (d2 >= BestDistSq) continue;
						if (LaneBlocked(Positions[j])) continue;   // ally in the way
						BestDistSq = d2;
						BestIdx    = j;
					}
					if (BestIdx != INDEX_NONE)
					{
						CF.TargetEntity       = Handles[BestIdx];
						CF.bHasAcquiredTarget = true;
						TargetSnapIdx         = BestIdx;
					}
				}

				// — Face the target + single-frame aiming line —
				if (CF.bHasAcquiredTarget && TargetSnapIdx != INDEX_NONE)
				{
					const FVector ToTarget = (Positions[TargetSnapIdx] - MyPos).GetSafeNormal2D();
					if (!ToTarget.IsNearlyZero())
					{
						FTransform T = Transforms[i].GetTransform();
						T.SetRotation(FRotationMatrix::MakeFromX(ToTarget).ToQuat());
						Transforms[i].GetMutableTransform() = T;
					}
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
