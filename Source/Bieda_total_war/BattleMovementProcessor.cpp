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
	// ── Spatial grid ──────────────────────────────────────────────────────────
	constexpr float MaxSeparationRadius = 200.f;  // cm
	constexpr float SeparationStrength  = 250.f;  // cm/s repulsion at contact distance

	// ── ETW fatigue thresholds (points) ───────────────────────────────────────
	constexpr int32 FatigueFreshMax     = 1799;   // 0–1799 = Fresh
	constexpr int32 FatigueActiveMax    = 3599;   // 1800–3599 = Active
	constexpr int32 FatigueWindedMax    = 7199;   // 3600–7199 = Winded
	constexpr int32 FatigueTiredMax     = 10799;  // 7200–10799 = Tired
	constexpr int32 FatigueVeryTiredMax = 21599;  // 10800–21599 = Very Tired
	constexpr int32 FatigueMax          = 28800;  // 21600+ = Exhausted

	// ── ETW fatigue accumulation rates (points/second) ────────────────────────
	constexpr int32 FatigueWalk        = 0;    // walking = neutral
	constexpr int32 FatigueRun         = 8;    // running
	constexpr int32 FatigueCharge      = 16;   // charging (bForceRun + melee contact)
	constexpr int32 FatigueCombat      = 10;   // in melee
	constexpr int32 FatigueShoot       = 8;    // shooting
	constexpr int32 FatigueReload      = 8;    // reloading
	constexpr int32 FatigueUnderFire   = 5;    // under small arms fire
	constexpr int32 FatigueIdle        = -8;   // resting idle (passive recovery)

	// ── Fatigue speed penalty by level (ETW style) ────────────────────────────
	// Applied multiplicatively to base march/run speed
	constexpr float SpeedPenaltyFresh     = 1.0f;
	constexpr float SpeedPenaltyActive    = 0.95f;
	constexpr float SpeedPenaltyWinded    = 0.88f;
	constexpr float SpeedPenaltyTired     = 0.78f;
	constexpr float SpeedPenaltyVeryTired = 0.65f;
	constexpr float SpeedPenaltyExhausted = 0.50f;

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
	MovementQuery.AddRequirement<FAgentCombatFragment>(EMassFragmentAccess::ReadOnly);
	MovementQuery.AddRequirement<FFatigueFragment>(EMassFragmentAccess::ReadWrite);
	MovementQuery.RegisterWithProcessor(*this);
}

// ── Fatigue level helper (stateless) ──────────────────────────────────────────
namespace
{
	EFatigueLevel FatigueLevelFromPoints(int32 Points)
	{
		if (Points <= FatigueFreshMax)     return EFatigueLevel::Fresh;
		if (Points <= FatigueActiveMax)    return EFatigueLevel::Active;
		if (Points <= FatigueWindedMax)    return EFatigueLevel::Winded;
		if (Points <= FatigueTiredMax)     return EFatigueLevel::Tired;
		if (Points <= FatigueVeryTiredMax) return EFatigueLevel::VeryTired;
		return EFatigueLevel::Exhausted;
	}

	float FatigueSpeedMultiplier(EFatigueLevel Level)
	{
		switch (Level)
		{
		case EFatigueLevel::Fresh:     return SpeedPenaltyFresh;
		case EFatigueLevel::Active:    return SpeedPenaltyActive;
		case EFatigueLevel::Winded:    return SpeedPenaltyWinded;
		case EFatigueLevel::Tired:     return SpeedPenaltyTired;
		case EFatigueLevel::VeryTired: return SpeedPenaltyVeryTired;
		case EFatigueLevel::Exhausted: return SpeedPenaltyExhausted;
		default:                       return SpeedPenaltyFresh;
		}
	}
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
	// Pass 2: compute movement + fatigue + separation + apply
	// -----------------------------------------------------------------------
	int32 GlobalIdx = 0;

	MovementQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& Ctx)
	{
		const float DeltaTime = Ctx.GetDeltaTimeSeconds() * BattleSimTimeScale();
		auto        Transforms = Ctx.GetMutableFragmentView<FTransformFragment>();
		const auto  States     = Ctx.GetFragmentView<FAgentStateFragment>();
		const auto  Orders     = Ctx.GetFragmentView<FOrderFragment>();
		auto        Velocities = Ctx.GetMutableFragmentView<FAgentVelocityFragment>();
		const auto  Morales    = Ctx.GetFragmentView<FMoraleFragment>();
		const auto  Combats    = Ctx.GetFragmentView<FAgentCombatFragment>();
		auto        Fatigues   = Ctx.GetMutableFragmentView<FFatigueFragment>();

		for (int32 i = 0; i < Ctx.GetNumEntities(); ++i, ++GlobalIdx)
		{
			const int32 LocalGlobal = GlobalIdx;  // index into Snaps/Positions
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
			const float PersonalSepRadius = Velocities[i].PersonalSeparationRadius;
			FVector Separation = FVector::ZeroVector;
			Grid.ForEachInRadius(CurrentPos, PersonalSepRadius, LocalGlobal,
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

			// ── Fatigue accumulation (ETW rates per-activity) ─────────────
			{
				int32 FatigueDelta = 0;
				switch (State)
				{
				case EAgentState::ADVANCING:
					if (Velocities[i].bForceRun)
						FatigueDelta = FatigueRun;
					else
						FatigueDelta = FatigueWalk;
					if (Velocities[i].bIsStraggler)
						FatigueDelta = FMath::Max(FatigueDelta, FatigueRun);  // stragglers always at run rate
					break;
				case EAgentState::MELEE:
					FatigueDelta = FatigueCombat;
					break;
				case EAgentState::LOADING:
				case EAgentState::AIMING:
				case EAgentState::FIRING:
					FatigueDelta = FatigueReload;
					break;
				case EAgentState::ROUTING:
				case EAgentState::RALLYING:
					FatigueDelta = FatigueRun;   // panicked running
					break;
				default: // HOLDING, STEADY, WAVERING, SHAKEN, PINNED
					FatigueDelta = FatigueIdle;
					break;
				}
				FatigueDelta -= Fatigues[i].Resistance;  // per-unit resistance
				Fatigues[i].FatiguePoints = FMath::Clamp(
					Fatigues[i].FatiguePoints + FMath::RoundToInt(FatigueDelta * DeltaTime),
					0, FatigueMax);
				Fatigues[i].Level = FatigueLevelFromPoints(Fatigues[i].FatiguePoints);
			}

			// ── Routing: flee AWAY FROM THE ENEMY ─────────────────────────
			if (State == EAgentState::ROUTING)
			{
				FVector& FleeDir = Velocities[i].FleeDirection;
				if (FleeDir.IsNearlyZero())
				{
					if (Orders[i].bHasFaceTarget)
					{
						FleeDir = (CurrentPos - Orders[i].FaceTarget).GetSafeNormal2D();
					}
					if (FleeDir.IsNearlyZero() && Orders[i].bHasTarget)
					{
						const FVector Away = CurrentPos - Orders[i].TargetPosition;
						if (Away.SizeSquared2D() > FMath::Square(200.f))
							FleeDir = Away.GetSafeNormal2D();
					}
					if (FleeDir.IsNearlyZero())
					{
						FleeDir = -Transforms[i].GetTransform().GetUnitAxis(EAxis::X);
						FleeDir = FleeDir.GetSafeNormal2D();
					}
					if (FleeDir.IsNearlyZero())
						FleeDir = FVector(0.f, 1.f, 0.f);
				}

				const float JitterX = 50.f * FMath::Sin(WorldTime * 3.f + Seed * 6.28f);
				const float JitterY = 50.f * FMath::Cos(WorldTime * 2.5f + Seed * 4.71f);

				const float FatMult = FatigueSpeedMultiplier(Fatigues[i].Level);
				const FVector Desired = FleeDir * RoutingSpeed * FatMult + Separation
					+ FVector(JitterX, JitterY, 0.f);
				Velocities[i].Velocity = FMath::VInterpTo(
					Velocities[i].Velocity, Desired, DeltaTime, 5.f);

				FTransform T = Transforms[i].GetTransform();
				T.SetLocation(CurrentPos + Velocities[i].Velocity * DeltaTime);
				T.SetRotation(FRotationMatrix::MakeFromX(FleeDir).ToQuat());
				Transforms[i].GetMutableTransform() = T;
				continue;
			}

			// Clear latched flee direction
			Velocities[i].FleeDirection = FVector::ZeroVector;

			// ── Morale-dependent fidget ───────────────────────────────────
			FVector FidgetOffset = FVector::ZeroVector;
			float   RotJitter    = 0.f;
			if (Morale < 70.f)
			{
				const float Strength = FMath::GetMappedRangeValueClamped(
					FVector2D(20.f, 70.f), FVector2D(1.f, 0.f), Morale);
				FidgetOffset = FVector(
					Strength * 25.f * FMath::Sin(WorldTime * 1.5f + Seed * 6.28f),
					Strength * 25.f * FMath::Cos(WorldTime * 1.2f + Seed * 3.14f),
					0.f);
				RotJitter = Strength * 15.f * FMath::Sin(WorldTime * 2.f + Seed * 4.71f);
			}

			// ── Stationary states: decelerate + separation + fidget ────────
			const bool bIsAdvancing = (State == EAgentState::ADVANCING && Orders[i].bHasTarget);
			if (!bIsAdvancing)
			{
				FVector DesiredVel = Separation + FidgetOffset * 2.f;

				constexpr float SettleRange = 250.f;
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

				if (Orders[i].bHasFaceTarget)
				{
					const FVector ToFace = (Orders[i].FaceTarget - T.GetLocation()).GetSafeNormal2D();
					if (!ToFace.IsNearlyZero())
					{
						const FQuat TargetRot = FRotationMatrix::MakeFromX(ToFace).ToQuat();
						const FQuat Blended = FQuat::Slerp(T.GetRotation(), TargetRot, FMath::Min(1.f, DeltaTime * 3.f));
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
				continue;
			}

			// ── Advancing: march with wavering ─────────────────────────────
			const FVector ToTarget = Orders[i].TargetPosition - CurrentPos;
			const float   DistToSlot = ToTarget.Size2D();

			if (DistToSlot > Velocities[i].PersonalSlotTolerance)
			{
				const FVector Dir = ToTarget.GetSafeNormal2D();
				const bool bIsLine = (Velocities[i].UnitType == EUnitType::LineInfantry);

				float BaseSpeed;
				if (Velocities[i].bIsStraggler)
					BaseSpeed = Velocities[i].CatchUpSpeed;
				else if (Velocities[i].bForceRun)
					BaseSpeed = Velocities[i].RunSpeed;
				else
					BaseSpeed = Velocities[i].MarchSpeed;

				// ETW fatigue speed penalty by level (not linear)
				BaseSpeed *= FatigueSpeedMultiplier(Fatigues[i].Level);

				const float WaverAmp = Velocities[i].PersonalWaverAmp;
				const float SpeedMul = 1.0f + WaverAmp * FMath::Sin(
					WorldTime * (0.7f + Seed * 0.6f) + Seed * 6.28f);

				const FVector LateralDir(-Dir.Y, Dir.X, 0.f);
				const float DriftAmp = Velocities[i].PersonalDriftAmp;
				const float LateralDrift = DriftAmp * FMath::Sin(
					WorldTime * (0.4f + Seed * 0.3f) + Seed * 4.71f);

				const float CurveAdj = bIsLine ? Velocities[i].CurveOffset : 0.f;

				const FVector Desired = Dir * BaseSpeed * SpeedMul
					+ LateralDir * (LateralDrift + CurveAdj * 0.1f)
					+ Separation + FidgetOffset;

				Velocities[i].Velocity = FMath::VInterpTo(
					Velocities[i].Velocity, Desired, DeltaTime, 5.f);

				FTransform T = Transforms[i].GetTransform();
				T.SetLocation(CurrentPos + Velocities[i].Velocity * DeltaTime);
				T.SetRotation(FRotationMatrix::MakeFromX(Dir).ToQuat());
				Transforms[i].GetMutableTransform() = T;
			}
			else
			{
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

				if (Orders[i].bHasFaceTarget)
				{
					const FVector ToFace = (Orders[i].FaceTarget - T.GetLocation()).GetSafeNormal2D();
					if (!ToFace.IsNearlyZero())
					{
						const FQuat TargetRot = FRotationMatrix::MakeFromX(ToFace).ToQuat();
						const FQuat Blended = FQuat::Slerp(T.GetRotation(), TargetRot, FMath::Min(1.f, DeltaTime * 3.f));
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
