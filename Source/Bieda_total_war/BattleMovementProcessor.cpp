#include "BattleMovementProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleSpatialGrid.h"
#include "BattleSimControl.h"
#include "BattleStats.h"

DECLARE_CYCLE_STAT(TEXT("Movement"), STAT_BiedaMovement, STATGROUP_Bieda);

namespace
{
	// Maximum separation radius — used for spatial grid query.
	// Each soldier uses its own VF.PersonalSeparationRadius (≤ this max).
	constexpr float MaxSeparationRadius = 200.f;  // cm
	constexpr float SeparationStrength  = 250.f;  // cm/s repulsion at contact distance
}

UBattleMovementProcessor::UBattleMovementProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;
}

void UBattleMovementProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	MovementQuery.Initialize(EntityManager);
	MovementQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadWrite);
	MovementQuery.AddRequirement<FAgentStateFragment>(EMassFragmentAccess::ReadOnly);
	MovementQuery.AddRequirement<FOrderFragment>(EMassFragmentAccess::ReadOnly);
	MovementQuery.AddRequirement<FAgentVelocityFragment>(EMassFragmentAccess::ReadWrite);
	MovementQuery.AddRequirement<FMoraleFragment>(EMassFragmentAccess::ReadOnly);
	MovementQuery.RegisterWithProcessor(*this);
}

void UBattleMovementProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	if (BattleSimPaused()) return;
	SCOPE_CYCLE_COUNTER(STAT_BiedaMovement);
	const float WorldTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// -----------------------------------------------------------------------
	// Pass 1: snapshot positions + alive/dead for spatial grid
	// -----------------------------------------------------------------------
	struct FAgentSnap { bool bDead; };
	TArray<FAgentSnap> Snaps;
	TArray<FVector>    Positions;

	MovementQuery.ForEachEntityChunk(Context, [&Snaps, &Positions](FMassExecutionContext& Ctx)
	{
		const auto Transforms = Ctx.GetFragmentView<FTransformFragment>();
		const auto States     = Ctx.GetFragmentView<FAgentStateFragment>();
		const int32 N = Ctx.GetNumEntities();
		Snaps.Reserve(Snaps.Num() + N);
		Positions.Reserve(Positions.Num() + N);
		for (int32 i = 0; i < N; ++i)
		{
			Positions.Add(Transforms[i].GetTransform().GetLocation());
			Snaps.Add({ States[i].State == EAgentState::DEAD });
		}
	});

	FBattleSpatialGrid Grid;
	Grid.Build(Positions, MaxSeparationRadius);

	// -----------------------------------------------------------------------
	// Pass 2: compute movement + separation + wavering and apply
	// -----------------------------------------------------------------------
	int32 GlobalIdx = 0;

	MovementQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& Ctx)
	{
		const float DeltaTime = Ctx.GetDeltaTimeSeconds();
		auto        Transforms = Ctx.GetMutableFragmentView<FTransformFragment>();
		const auto  States     = Ctx.GetFragmentView<FAgentStateFragment>();
		const auto  Orders     = Ctx.GetFragmentView<FOrderFragment>();
		auto        Velocities = Ctx.GetMutableFragmentView<FAgentVelocityFragment>();
		const auto  Morales    = Ctx.GetFragmentView<FMoraleFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i, ++GlobalIdx)
		{
			const EAgentState State = States[i].State;

			if (State == EAgentState::DEAD)
			{
				Velocities[i].Velocity = FVector::ZeroVector;
				continue;
			}

			const FVector CurrentPos = Transforms[i].GetTransform().GetLocation();
			const float   Seed       = Velocities[i].NoiseSeed;
			const float   Morale     = Morales[i].Morale;

			// ── Separation force (spatial grid query) ─────────────────────
			// Each soldier uses its own personal space radius (set at spawn).
			const float PersonalSepRadius = Velocities[i].PersonalSeparationRadius;
			FVector Separation = FVector::ZeroVector;
			Grid.ForEachInRadius(CurrentPos, PersonalSepRadius, GlobalIdx,
				[&](int32 j, float DistSq2D)
				{
					if (Snaps[j].bDead) return;
					const float Dist = FMath::Sqrt(DistSq2D);
					if (Dist > 1.f)
					{
						const FVector Diff = CurrentPos - Positions[j];
						const float Weight = 1.f - (Dist / PersonalSepRadius);
						Separation += Diff.GetSafeNormal2D() * Weight * SeparationStrength;
					}
				});

			// ── Routing: flee AWAY FROM THE ENEMY ─────────────────────────
			if (State == EAgentState::ROUTING)
			{
				// Latch the flee direction ONCE on the first routing frame.
				// This avoids two bugs:
				//   1) TargetPosition ≈ CurrentPos (after halt) → zero vector
				//      → soldier drifts on jitter alone (random direction)
				//   2) Fidget rotated the soldier's facing pre-panic → -forward
				//      points sideways or even toward the enemy.
				// By capturing once, the direction stays stable for the whole rout.
				FVector& FleeDir = Velocities[i].FleeDirection;
				if (FleeDir.IsNearlyZero())
				{
					// Priority 1: flee away from known enemy position (engagement)
					if (Orders[i].bHasFaceTarget)
					{
						FleeDir = (CurrentPos - Orders[i].FaceTarget).GetSafeNormal2D();
					}
					// Priority 2: flee away from formation target (if far enough
					// to give a meaningful direction — not a halt-in-place slot)
					if (FleeDir.IsNearlyZero() && Orders[i].bHasTarget)
					{
						const FVector Away = CurrentPos - Orders[i].TargetPosition;
						if (Away.SizeSquared2D() > FMath::Square(200.f))
							FleeDir = Away.GetSafeNormal2D();
					}
					// Priority 3: flee opposite of current facing (last resort —
					// only for soldiers who panicked outside of combat/movement)
					if (FleeDir.IsNearlyZero())
					{
						FleeDir = -Transforms[i].GetTransform().GetUnitAxis(EAxis::X);
						FleeDir = FleeDir.GetSafeNormal2D();
					}
					// Final safety: if STILL zero (degenerate transform), pick +Y
					if (FleeDir.IsNearlyZero())
						FleeDir = FVector(0.f, 1.f, 0.f);
				}

				// Chaotic jitter (adds nervous weaving, not a new heading)
				const float JitterX = 50.f * FMath::Sin(WorldTime * 3.f + Seed * 6.28f);
				const float JitterY = 50.f * FMath::Cos(WorldTime * 2.5f + Seed * 4.71f);

				const FVector Desired = FleeDir * RoutingSpeed + Separation
					+ FVector(JitterX, JitterY, 0.f);
				Velocities[i].Velocity = FMath::VInterpTo(
					Velocities[i].Velocity, Desired, DeltaTime, 5.f);

				FTransform T = Transforms[i].GetTransform();
				T.SetLocation(CurrentPos + Velocities[i].Velocity * DeltaTime);
				T.SetRotation(FRotationMatrix::MakeFromX(FleeDir).ToQuat());
				Transforms[i].GetMutableTransform() = T;
				continue;
			}

			// Clear latched flee direction whenever NOT routing (so a fresh
			// rout episode gets a fresh direction capture).
			Velocities[i].FleeDirection = FVector::ZeroVector;

			// ── Morale-dependent fidget (stationary states) ───────────────
			// Low morale → soldiers shift nervously, look around
			FVector FidgetOffset = FVector::ZeroVector;
			float   RotJitter    = 0.f;
			if (Morale < 70.f)
			{
				// Fidget strength: 0 at morale=70, max at morale=20
				const float Strength = FMath::GetMappedRangeValueClamped(
					FVector2D(20.f, 70.f), FVector2D(1.f, 0.f), Morale);

				FidgetOffset = FVector(
					Strength * 25.f * FMath::Sin(WorldTime * 1.5f + Seed * 6.28f),
					Strength * 25.f * FMath::Cos(WorldTime * 1.2f + Seed * 3.14f),
					0.f);

				// Rotation jitter: nervous head turns (±15° at lowest morale)
				RotJitter = Strength * 15.f * FMath::Sin(WorldTime * 2.f + Seed * 4.71f);
			}

			// ── Stationary states: decelerate + separation + fidget ────────
			if (State != EAgentState::ADVANCING || !Orders[i].bHasTarget)
			{
				FVector DesiredVel = Separation + FidgetOffset * 2.f;

				// Snap-to-FinalPos drift: when soldier is HOLDING after reaching
				// his slot (state processor flipped him from ADVANCING at ~150 cm),
				// keep nudging him toward TargetPosition + PersonalFinalOffset so
				// he settles into HIS spot, not THE spot. Without this, formation
				// collapses into a grid after a few orders.
				//
				// CRITICAL — the DistToFinal < SettleRange gate:
				//   This snap is ONLY for short-range settling. A soldier is ALSO
				//   HOLDING with bHasTarget during the order-propagation window of a
				//   FRESH move order (before BattleOrderProcessor flips him to
				//   ADVANCING). In that window the target is far away (thousands of
				//   cm), so an ungated SnapVel = DistToFinal / SnapTime would be
				//   ~2500 cm/s — soldiers would "black-hole sprint" toward the goal
				//   instead of marching. The state processor only flips
				//   ADVANCING→HOLDING within 150 cm of the slot, so genuine settling
				//   is always < ~210 cm (150 + max PersonalFinalOffset). Anything
				//   farther is a pending march and must be left to the ADVANCING code.
				constexpr float SettleRange = 250.f;   // cm
				if (State == EAgentState::HOLDING && Orders[i].bHasTarget)
				{
					const FVector FinalPos    = Orders[i].TargetPosition + Velocities[i].PersonalFinalOffset;
					const FVector ToFinal     = FinalPos - CurrentPos;
					const float   DistToFinal = ToFinal.Size2D();
					if (DistToFinal > 5.f && DistToFinal < SettleRange
						&& Velocities[i].PersonalSnapTime > KINDA_SMALL_NUMBER)
					{
						const FVector SnapVel = ToFinal.GetSafeNormal2D()
							* (DistToFinal / Velocities[i].PersonalSnapTime);
						DesiredVel += SnapVel;
					}
				}

				Velocities[i].Velocity = FMath::VInterpTo(
					Velocities[i].Velocity, DesiredVel, DeltaTime, 5.f);

				FTransform T = Transforms[i].GetTransform();
				T.SetLocation(T.GetLocation() + Velocities[i].Velocity * DeltaTime);

				// ── Face engagement target (smooth rotation) ──────────────
				if (Orders[i].bHasFaceTarget)
				{
					const FVector ToFace = (Orders[i].FaceTarget - T.GetLocation()).GetSafeNormal2D();
					if (!ToFace.IsNearlyZero())
					{
						const FQuat TargetRot = FRotationMatrix::MakeFromX(ToFace).ToQuat();
						const FQuat Blended   = FQuat::Slerp(T.GetRotation(), TargetRot, FMath::Min(1.f, DeltaTime * 3.f));
						T.SetRotation(Blended);
					}
				}
				// Apply rotation jitter (nervous looking around)
				else if (FMath::Abs(RotJitter) > 0.5f)
				{
					FRotator Rot = T.Rotator();
					Rot.Yaw += RotJitter;
					T.SetRotation(Rot.Quaternion());
				}

				Transforms[i].GetMutableTransform() = T;
				continue;
			}

			// ── Advancing: march with wavering ─────────────────────────────
			const FVector ToTarget = Orders[i].TargetPosition - CurrentPos;
			const float   DistToSlot = ToTarget.Size2D();

			if (DistToSlot > Velocities[i].PersonalSlotTolerance)
			{
				const FVector Dir = ToTarget.GetSafeNormal2D();
				const bool bIsLine = (Velocities[i].UnitType == EUnitType::LineInfantry);

				// ── Choose speed: 3-tier system ───────────────────────────
				// Priority 1: bIsStraggler  → CatchUpSpeed (auto, NCO grabbed him)
				// Priority 2: bForceRun     → RunSpeed (UI: Bieg)
				// Priority 3: default       → MarchSpeed (UI: Marsz)
				float BaseSpeed;
				if (Velocities[i].bIsStraggler)
				{
					BaseSpeed = Velocities[i].CatchUpSpeed;
				}
				else if (Velocities[i].bForceRun)
				{
					BaseSpeed = Velocities[i].RunSpeed;
				}
				else
				{
					BaseSpeed = Velocities[i].MarchSpeed;
				}

				// ── Speed wavering — per-soldier amplitude ────────────────
				const float WaverAmp = Velocities[i].PersonalWaverAmp;
				const float SpeedMul = 1.0f + WaverAmp * FMath::Sin(
					WorldTime * (0.7f + Seed * 0.6f) + Seed * 6.28f);

				// ── Lateral drift — per-soldier amplitude ─────────────────
				const FVector LateralDir(-Dir.Y, Dir.X, 0.f);
				const float DriftAmp = Velocities[i].PersonalDriftAmp;
				const float LateralDrift = DriftAmp * FMath::Sin(
					WorldTime * (0.4f + Seed * 0.3f) + Seed * 4.71f);

				// ── Formation curve offset (line infantry) ────────────────
				const float CurveAdj = bIsLine ? Velocities[i].CurveOffset : 0.f;

				const FVector Desired = Dir * BaseSpeed * SpeedMul
					+ LateralDir * (LateralDrift + CurveAdj * 0.1f)
					+ Separation
					+ FidgetOffset;

				Velocities[i].Velocity = FMath::VInterpTo(
					Velocities[i].Velocity, Desired, DeltaTime, 5.f);

				FTransform T = Transforms[i].GetTransform();
				T.SetLocation(CurrentPos + Velocities[i].Velocity * DeltaTime);
				T.SetRotation(FRotationMatrix::MakeFromX(Dir).ToQuat());
				Transforms[i].GetMutableTransform() = T;
			}
			else
			{
				// ── Within personal tolerance: snap-to-FINAL-OFFSET soft drift ──
				// Drift toward TargetPosition + PersonalFinalOffset — soldier's
				// permanent quirk position. The line tightens up but never to a
				// perfect grid: each man stops at HIS spot, not at THE spot.
				const FVector FinalPos    = Orders[i].TargetPosition + Velocities[i].PersonalFinalOffset;
				const FVector ToFinal     = FinalPos - CurrentPos;
				const float   DistToFinal = ToFinal.Size2D();

				FVector DesiredVel = Separation + FidgetOffset * 2.f;
				if (DistToFinal > 5.f && Velocities[i].PersonalSnapTime > KINDA_SMALL_NUMBER)
				{
					const FVector SnapVel = ToFinal.GetSafeNormal2D()
						* (DistToFinal / Velocities[i].PersonalSnapTime);
					DesiredVel += SnapVel;
				}

				Velocities[i].Velocity = FMath::VInterpTo(
					Velocities[i].Velocity, DesiredVel, DeltaTime, 5.f);

				FTransform T = Transforms[i].GetTransform();
				T.SetLocation(T.GetLocation() + Velocities[i].Velocity * DeltaTime);

				// Face engagement target or fidget
				if (Orders[i].bHasFaceTarget)
				{
					const FVector ToFace = (Orders[i].FaceTarget - T.GetLocation()).GetSafeNormal2D();
					if (!ToFace.IsNearlyZero())
					{
						const FQuat TargetRot = FRotationMatrix::MakeFromX(ToFace).ToQuat();
						const FQuat Blended   = FQuat::Slerp(T.GetRotation(), TargetRot, FMath::Min(1.f, DeltaTime * 3.f));
						T.SetRotation(Blended);
					}
				}
				else if (FMath::Abs(RotJitter) > 0.5f)
				{
					FRotator Rot = T.Rotator();
					Rot.Yaw += RotJitter;
					T.SetRotation(Rot.Quaternion());
				}

				Transforms[i].GetMutableTransform() = T;
			}
		}
	});
}
