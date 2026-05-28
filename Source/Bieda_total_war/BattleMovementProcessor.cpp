#include "BattleMovementProcessor.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "BattleTypes.h"
#include "BattleSpatialGrid.h"

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

			// ── Routing: flee chaotically ──────────────────────────────────
			if (State == EAgentState::ROUTING)
			{
				FVector FleeDir = Orders[i].bHasTarget
					? (CurrentPos - Orders[i].TargetPosition).GetSafeNormal2D()
					: -Transforms[i].GetTransform().GetUnitAxis(EAxis::X);

				// Chaotic jitter while routing
				const float JitterX = 50.f * FMath::Sin(WorldTime * 3.f + Seed * 6.28f);
				const float JitterY = 50.f * FMath::Cos(WorldTime * 2.5f + Seed * 4.71f);

				const FVector Desired = FleeDir * RoutingSpeed + Separation
					+ FVector(JitterX, JitterY, 0.f);
				Velocities[i].Velocity = FMath::VInterpTo(
					Velocities[i].Velocity, Desired, DeltaTime, 5.f);

				FTransform T = Transforms[i].GetTransform();
				T.SetLocation(CurrentPos + Velocities[i].Velocity * DeltaTime);
				if (!FleeDir.IsNearlyZero())
					T.SetRotation(FRotationMatrix::MakeFromX(FleeDir).ToQuat());
				Transforms[i].GetMutableTransform() = T;
				continue;
			}

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
				const FVector DesiredVel = Separation + FidgetOffset * 2.f;
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
				// ── Within personal tolerance: snap-to-slot soft drift ────
				// Soldier slowly drifts toward exact slot position over PersonalSnapTime,
				// so the line tightens up after the formation halts instead of leaving
				// each man stopped wherever he happened to cross his tolerance.
				FVector DesiredVel = Separation + FidgetOffset * 2.f;
				if (DistToSlot > 5.f && Velocities[i].PersonalSnapTime > KINDA_SMALL_NUMBER)
				{
					const FVector SnapVel = ToTarget.GetSafeNormal2D()
						* (DistToSlot / Velocities[i].PersonalSnapTime);
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
