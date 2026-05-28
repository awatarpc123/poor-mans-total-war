#include "BattleSpawnerActor.h"
#include "MassEntitySubsystem.h"
#include "MassEntityManager.h"
#include "MassCommonFragments.h"
#include "BattleTypes.h"
#include "DrawDebugHelpers.h"

namespace { uint8 GNextSquadId = 0; }

ABattleSpawnerActor::ABattleSpawnerActor()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
}

void ABattleSpawnerActor::BeginPlay()
{
	Super::BeginPlay();
	SetupVisualization();
	SpawnAgents();
}

void ABattleSpawnerActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateEngagement();
	UpdateVolley();
	UpdateStragglers();
	UpdateVisualization();
}

void ABattleSpawnerActor::UpdateStragglers()
{
	// A soldier is a "straggler" when they are significantly farther from
	// their formation slot than the rest of the squad. This is invariant to
	// formation shape (rectangle, line, square) because every soldier is
	// measured against their OWN target slot, not against some shared point.
	//
	// Hysteresis offsets (relative to formation average) prevent flicker.
	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	// ── Pass 1: average DistToSlot across alive non-routing soldiers ─────────
	float TotalDist = 0.f;
	int32 CountedSoldiers = 0;

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State == EAgentState::DEAD || SF.State == EAgentState::ROUTING) continue;

		const FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
		if (!OF.bHasTarget) continue;   // no target = nothing to measure

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
		const float Dist = FVector::Dist2D(TF.GetTransform().GetLocation(), OF.TargetPosition);

		TotalDist += Dist;
		++CountedSoldiers;
	}

	if (CountedSoldiers == 0) return;
	const float AvgDist = TotalDist / static_cast<float>(CountedSoldiers);

	// ── Pass 2: flag soldiers whose DistToSlot >> formation average ──────────
	// Enter:  DistToSlot > Avg + 600 cm  (~6 m behind the rest)
	// Exit:   DistToSlot < Avg + 150 cm  (caught up to within a row or two)
	constexpr float EnterOffset = 600.f;
	constexpr float ExitOffset  = 150.f;

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State == EAgentState::DEAD || SF.State == EAgentState::ROUTING) continue;

		FAgentVelocityFragment& VF = EM.GetFragmentDataChecked<FAgentVelocityFragment>(Entity);
		const FOrderFragment&   OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);

		if (!OF.bHasTarget)
		{
			VF.bIsStraggler = false;   // no target → not a straggler
			continue;
		}

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
		const float Dist   = FVector::Dist2D(TF.GetTransform().GetLocation(), OF.TargetPosition);
		const float Excess = Dist - AvgDist;

		if (!VF.bIsStraggler && Excess > EnterOffset)
		{
			VF.bIsStraggler = true;
		}
		else if (VF.bIsStraggler && Excess < ExitOffset)
		{
			VF.bIsStraggler = false;
		}
	}
}

void ABattleSpawnerActor::SpawnAgents()
{
	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!ensureMsgf(Subsystem, TEXT("BattleSpawner: brak UMassEntitySubsystem")))
		return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	MySquadId = GNextSquadId++;

	TArray<const UScriptStruct*> Fragments = {
		FTransformFragment::StaticStruct(),
		FAgentStateFragment::StaticStruct(),
		FMoraleFragment::StaticStruct(),
		FOrderFragment::StaticStruct(),
		FAgentVelocityFragment::StaticStruct(),
		FAgentCombatFragment::StaticStruct(),
		FOrderPropagationFragment::StaticStruct(),
		FFactionFragment::StaticStruct()
	};
	FMassArchetypeHandle Archetype = EM.CreateArchetype(Fragments);

	const FVector Base = GetActorLocation();

	// RowSize: 0 = auto (square grid), otherwise use value from editor
	const int32 EffectiveRowSize = (RowSize > 0)
		? FMath::Clamp(RowSize, 1, NumAgents)
		: FMath::Max(1, FMath::CeilToInt(FMath::Sqrt((float)NumAgents)));
	CurrentRowSize = EffectiveRowSize;   // store for future quick-move orders
	const float HalfGrid = EffectiveRowSize * SpawnSpacing * 0.5f;

	SpawnedEntities.Reserve(NumAgents);

	for (int32 i = 0; i < NumAgents; ++i)
	{
		const int32 Row = i / EffectiveRowSize;
		const int32 Col = i % EffectiveRowSize;
		const FVector SpawnPos = Base
			+ FVector(Col * SpawnSpacing - HalfGrid, Row * SpawnSpacing - HalfGrid, 0.f);

		FMassEntityHandle Entity = EM.CreateEntity(Archetype);

		FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
		TF.GetMutableTransform() = FTransform(SpawnFacing.Quaternion(), SpawnPos, FVector::OneVector);

		FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		SF.State = EAgentState::HOLDING;

		FMoraleFragment& MF = EM.GetFragmentDataChecked<FMoraleFragment>(Entity);
		MF.Morale = InitialMorale;

		FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
		OF.TargetPosition = FVector::ZeroVector;
		OF.bHasTarget     = false;

		FAgentVelocityFragment& VF = EM.GetFragmentDataChecked<FAgentVelocityFragment>(Entity);
		VF.NoiseSeed     = FMath::FRand();
		VF.UnitType      = UnitType;
		VF.MarchSpeed    = MarchSpeed;
		VF.RunSpeed      = RunSpeed;
		VF.CatchUpSpeed  = CatchUpSpeed;
		VF.bForceRun     = false;       // start in Marsz mode
		VF.bIsStraggler  = false;       // set true later when soldier lags behind
		VF.FormationRow  = Row;
		VF.FormationCol  = Col;

		// Organic curve: sin-based arc + per-entity wobble
		{
			const float ColNorm = (EffectiveRowSize > 1)
				? static_cast<float>(Col) / static_cast<float>(EffectiveRowSize - 1) : 0.5f;
			const float ArcOffset = FormationCurveStrength
				* FMath::Sin(ColNorm * UE_PI);  // arc — bulges in the middle
			const float SWobble = FormationCurveStrength * 0.3f
				* FMath::Sin(ColNorm * UE_PI * 2.f + VF.NoiseSeed * 6.28f);  // S-wobble
			VF.CurveOffset = ArcOffset + SWobble;
		}

		FAgentCombatFragment& CF = EM.GetFragmentDataChecked<FAgentCombatFragment>(Entity);
		CF.HP                 = SoldierHP;
		CF.FireRange          = SoldierFireRange;
		CF.VisionHalfAngleDeg = VisionHalfAngleDeg;
		CF.VolleyMode         = VolleyMode;
		CF.bVolleyReady       = false;
		CF.bVolleySignal      = false;

		// Line infantry: tighter reload variance; militia: wider
		if (UnitType == EUnitType::LineInfantry)
		{
			CF.ReloadDuration = SoldierReloadTime * FMath::RandRange(0.9f, 1.1f);
			CF.Accuracy       = SoldierAccuracy   * FMath::RandRange(0.85f, 1.15f);
		}
		else
		{
			CF.ReloadDuration = SoldierReloadTime * FMath::RandRange(0.8f, 1.2f);
			CF.Accuracy       = SoldierAccuracy   * FMath::RandRange(0.75f, 1.25f);
		}

		FFactionFragment& FF = EM.GetFragmentDataChecked<FFactionFragment>(Entity);
		FF.TeamId  = TeamId;
		FF.SquadId = MySquadId;

		SpawnedEntities.Add(Entity);
	}

	// ── Officer (front of formation) ──────────────────────────────────────────
	if (bSpawnOfficer)
	{
		TArray<const UScriptStruct*> OfficerFrags = {
			FTransformFragment::StaticStruct(),
			FMoraleFragment::StaticStruct(),
			FOfficerFragment::StaticStruct(),
			FFactionFragment::StaticStruct()
		};
		FMassArchetypeHandle OfficerArchetype = EM.CreateArchetype(OfficerFrags);
		OfficerEntity = EM.CreateEntity(OfficerArchetype);

		// Officer stands at the FRONT of the formation (first row center)
		const FVector OfficerPos = Base + SpawnFacing.Quaternion().RotateVector(
			FVector(0.f, EffectiveRowSize * SpawnSpacing * 0.5f + 100.f, 0.f));

		FTransformFragment& OTF = EM.GetFragmentDataChecked<FTransformFragment>(OfficerEntity);
		OTF.GetMutableTransform() = FTransform(SpawnFacing.Quaternion(), OfficerPos, FVector::OneVector);

		FMoraleFragment& OMF = EM.GetFragmentDataChecked<FMoraleFragment>(OfficerEntity);
		OMF.Morale = OfficerInitialMorale;

		FOfficerFragment& OFF = EM.GetFragmentDataChecked<FOfficerFragment>(OfficerEntity);
		OFF.bIsAlive            = true;
		OFF.bJustDied           = false;
		OFF.FormationFrontPos   = OfficerPos;
		OFF.bHasFormationTarget = true;
		OFF.MoveSpeed           = MarchSpeed;   // start in Marsz; SetForceRun updates this

		FFactionFragment& OFF_F = EM.GetFragmentDataChecked<FFactionFragment>(OfficerEntity);
		OFF_F.TeamId  = TeamId;
		OFF_F.SquadId = MySquadId;
	}

	// ── NCOs (podoficerowie — behind/flanks of formation) ─────────────────────
	if (NumNCOs > 0)
	{
		TArray<const UScriptStruct*> NCOFrags = {
			FTransformFragment::StaticStruct(),
			FMoraleFragment::StaticStruct(),
			FNCOFragment::StaticStruct(),
			FFactionFragment::StaticStruct()
		};
		FMassArchetypeHandle NCOArchetype = EM.CreateArchetype(NCOFrags);
		NCOEntities.Reserve(NumNCOs);

		for (int32 n = 0; n < NumNCOs; ++n)
		{
			FMassEntityHandle NCOEntity = EM.CreateEntity(NCOArchetype);

			// Spread NCOs behind the last row
			const float LateralOffset = (n - (NumNCOs - 1) * 0.5f) * SpawnSpacing * 2.f;
			const FVector NCOLocalPos(LateralOffset, -(EffectiveRowSize * SpawnSpacing * 0.5f + 200.f), 0.f);
			const FVector NCOPos = Base + SpawnFacing.Quaternion().RotateVector(NCOLocalPos);

			FTransformFragment& NTF = EM.GetFragmentDataChecked<FTransformFragment>(NCOEntity);
			NTF.GetMutableTransform() = FTransform(SpawnFacing.Quaternion(), NCOPos, FVector::OneVector);

			FMoraleFragment& NMF = EM.GetFragmentDataChecked<FMoraleFragment>(NCOEntity);
			NMF.Morale = OfficerInitialMorale;   // NCOs have officer-level morale

			FNCOFragment& NCO = EM.GetFragmentDataChecked<FNCOFragment>(NCOEntity);
			NCO.bIsAlive        = true;
			NCO.FormationPos    = NCOPos;
			NCO.bHasFormationPos = true;
			NCO.MoveSpeed       = MarchSpeed;     // formation pace (SetForceRun syncs this)
			NCO.ChaseSpeed      = CatchUpSpeed;   // when chasing stragglers/routing

			FFactionFragment& NFF = EM.GetFragmentDataChecked<FFactionFragment>(NCOEntity);
			NFF.TeamId  = TeamId;
			NFF.SquadId = MySquadId;

			NCOEntities.Add(NCOEntity);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("BattleSpawner: %d soldiers + officer=%d + NCOs=%d  squad=%d team=%d rowSize=%d"),
		SpawnedEntities.Num(), bSpawnOfficer ? 1 : 0, NCOEntities.Num(),
		MySquadId, TeamId, CurrentRowSize);
}

void ABattleSpawnerActor::IssueMoveOrder(const FVector& NewWorldTarget, int32 InRowSize,
	const FVector& InFrontLineDir)
{
	// Ground move cancels any active engagement
	EngagedTarget = nullptr;
	ClearFaceTarget();

	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	// ── Determine row size ────────────────────────────────────────────────────
	int32 LocalRowSize;
	if (InRowSize > 0)
	{
		LocalRowSize = FMath::Clamp(InRowSize, 3, NumAgents);
		CurrentRowSize = LocalRowSize;
	}
	else if (CurrentRowSize > 0)
	{
		LocalRowSize = CurrentRowSize;
	}
	else
	{
		LocalRowSize = FMath::Max(3, FMath::CeilToInt(FMath::Sqrt((float)NumAgents)));
		CurrentRowSize = LocalRowSize;
	}

	const int32 NumRows   = FMath::Max(1, FMath::CeilToInt((float)NumAgents / LocalRowSize));
	const float HalfFront = LocalRowSize * SpawnSpacing * 0.5f;
	const float HalfDepth = NumRows * SpawnSpacing * 0.5f;

	// ── Determine front-line direction ────────────────────────────────────────
	// FrontLineDir = direction soldiers spread along (left-to-right of formation)
	// Facing = perpendicular to front line
	FVector FrontLineDir;
	if (!InFrontLineDir.IsNearlyZero())
	{
		// Drag-to-form: use the drag direction directly
		FrontLineDir = InFrontLineDir.GetSafeNormal2D();
	}
	else
	{
		// Quick click: front line is perpendicular to march direction
		const FVector Center   = GetFormationCenter();
		const FVector MarchDir = (NewWorldTarget - Center).GetSafeNormal2D();
		if (MarchDir.IsNearlyZero())
			FrontLineDir = FVector(1.f, 0.f, 0.f);
		else
			FrontLineDir = FVector(MarchDir.Y, -MarchDir.X, 0.f);  // 90° CW
	}

	// Build rotation: X = front line direction, Y = depth direction
	const FQuat FormRot = FRotationMatrix::MakeFromX(FrontLineDir).ToQuat();

	// ── Assign each soldier a formation slot ──────────────────────────────────
	const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());
	for (int32 i = 0; i < SoldierCount; ++i)
	{
		const FMassEntityHandle Entity = SpawnedEntities[i];
		if (!EM.IsEntityValid(Entity)) continue;

		const int32 Col = i % LocalRowSize;
		const int32 Row = i / LocalRowSize;

		// Col = position along front line (X of rotation)
		// Row = depth, front row (Row=0) at +Y, back rows toward -Y
		const FVector LocalOffset(
			Col * SpawnSpacing - HalfFront,
			HalfDepth - Row * SpawnSpacing,
			0.f
		);
		const FVector SlotPos = NewWorldTarget + FormRot.RotateVector(LocalOffset);

		FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
		OF.TargetPosition  = SlotPos;
		OF.bHasTarget      = true;

		// Update formation slot info for line infantry movement
		FAgentVelocityFragment& VF = EM.GetFragmentDataChecked<FAgentVelocityFragment>(Entity);
		VF.FormationRow = Row;
		VF.FormationCol = Col;

		// Recompute organic curve offset
		{
			const float ColNorm = (LocalRowSize > 1)
				? static_cast<float>(Col) / static_cast<float>(LocalRowSize - 1) : 0.5f;
			VF.CurveOffset = FormationCurveStrength * FMath::Sin(ColNorm * UE_PI)
				+ FormationCurveStrength * 0.3f
				  * FMath::Sin(ColNorm * UE_PI * 2.f + VF.NoiseSeed * 6.28f);
		}

		FOrderPropagationFragment& PF = EM.GetFragmentDataChecked<FOrderPropagationFragment>(Entity);
		PF.bOrderReceived = false;
		PF.bOrderExecuted = false;
		PF.bOrderIgnored  = false;
		PF.ExecutionTimer = 0.f;
		PF.ExecutionDelay = 0.f;

		// Reset volley state on new order
		FAgentCombatFragment& CF = EM.GetFragmentDataChecked<FAgentCombatFragment>(Entity);
		CF.bVolleyReady  = false;
		CF.bVolleySignal = false;

		FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State != EAgentState::DEAD)
		{
			SF.State      = EAgentState::HOLDING;
			SF.StateTimer = 0.f;
		}
	}

	// ── Update officer front position ─────────────────────────────────────────
	if (OfficerEntity.IsValid() && EM.IsEntityValid(OfficerEntity))
	{
		FOfficerFragment& OFF = EM.GetFragmentDataChecked<FOfficerFragment>(OfficerEntity);
		const FVector FrontLocal(0.f, HalfDepth + 100.f, 0.f);
		OFF.FormationFrontPos   = NewWorldTarget + FormRot.RotateVector(FrontLocal);
		OFF.bHasFormationTarget = true;
	}

	// ── Update NCO formation positions (behind last row) ──────────────────────
	for (int32 n = 0; n < NCOEntities.Num(); ++n)
	{
		if (!EM.IsEntityValid(NCOEntities[n])) continue;
		FNCOFragment& NCO = EM.GetFragmentDataChecked<FNCOFragment>(NCOEntities[n]);
		const float LateralOffset = (n - (NCOEntities.Num() - 1) * 0.5f) * SpawnSpacing * 2.f;
		const FVector RearLocal(LateralOffset, -(HalfDepth + 200.f), 0.f);
		NCO.FormationPos     = NewWorldTarget + FormRot.RotateVector(RearLocal);
		NCO.bHasFormationPos = true;
		NCO.bHasTarget       = false;   // clear stale target on new order
	}

	UE_LOG(LogTemp, Log, TEXT("BattleSpawner squad=%d: marsz → (%.0f, %.0f) rowSize=%d rows=%d"),
		MySquadId, NewWorldTarget.X, NewWorldTarget.Y, LocalRowSize, NumRows);
}

void ABattleSpawnerActor::HoldPosition()
{
	// Cancel engagement — prevents UpdateEngagement from repositioning soldiers
	EngagedTarget = nullptr;
	ClearFaceTarget();

	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	// ── Stop soldiers ────────────────────────────────────────────────────────
	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;

		FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State == EAgentState::DEAD) continue;

		if (SF.State == EAgentState::ADVANCING)
		{
			SF.State      = EAgentState::HOLDING;
			SF.StateTimer = 0.f;
		}

		FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
		OF.bHasTarget = false;
	}

	// ── Stop officer ─────────────────────────────────────────────────────────
	if (OfficerEntity.IsValid() && EM.IsEntityValid(OfficerEntity))
	{
		FOfficerFragment& OFF = EM.GetFragmentDataChecked<FOfficerFragment>(OfficerEntity);
		OFF.bHasFormationTarget = false;
	}

	// ── Stop NCOs ────────────────────────────────────────────────────────────
	for (const FMassEntityHandle& NCO : NCOEntities)
	{
		if (!EM.IsEntityValid(NCO)) continue;
		FNCOFragment& NF = EM.GetFragmentDataChecked<FNCOFragment>(NCO);
		NF.bHasFormationPos = false;
		NF.bHasTarget       = false;
	}
}

FVector ABattleSpawnerActor::GetFormationCenter() const
{
	UWorld* World = GetWorld();
	if (!World) return GetActorLocation();

	const UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return GetActorLocation();

	const FMassEntityManager& EM = Subsystem->GetEntityManager();

	FVector Sum = FVector::ZeroVector;
	int32 Count = 0;

	const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());
	for (int32 i = 0; i < SoldierCount; ++i)
	{
		const FMassEntityHandle Entity = SpawnedEntities[i];
		if (!EM.IsEntityValid(Entity)) continue;

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State == EAgentState::DEAD) continue;

		Sum += TF.GetTransform().GetLocation();
		++Count;
	}

	return (Count > 0) ? (Sum / static_cast<float>(Count)) : GetActorLocation();
}

bool ABattleSpawnerActor::HasAliveSoldiers() const
{
	UWorld* World = GetWorld();
	if (!World) return false;

	const UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return false;

	const FMassEntityManager& EM = Subsystem->GetEntityManager();

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State != EAgentState::DEAD) return true;
	}
	return false;
}

int32 ABattleSpawnerActor::GetAliveCount() const
{
	UWorld* World = GetWorld();
	if (!World) return 0;

	const UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return 0;

	const FMassEntityManager& EM = Subsystem->GetEntityManager();
	int32 Count = 0;

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State != EAgentState::DEAD) ++Count;
	}
	return Count;
}

float ABattleSpawnerActor::GetAverageMorale() const
{
	UWorld* World = GetWorld();
	if (!World) return 0.f;

	const UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return 0.f;

	const FMassEntityManager& EM = Subsystem->GetEntityManager();
	float Sum = 0.f;
	int32 Count = 0;

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State == EAgentState::DEAD) continue;
		const FMoraleFragment& MF = EM.GetFragmentDataChecked<FMoraleFragment>(Entity);
		Sum += MF.Morale;
		++Count;
	}
	return (Count > 0) ? (Sum / static_cast<float>(Count)) : 0.f;
}

FString ABattleSpawnerActor::GetDominantStateString() const
{
	UWorld* World = GetWorld();
	if (!World) return TEXT("---");

	const UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return TEXT("---");

	const FMassEntityManager& EM = Subsystem->GetEntityManager();

	// Count each state
	int32 StateCounts[10] = {};
	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State == EAgentState::DEAD) continue;
		StateCounts[static_cast<uint8>(SF.State)]++;
	}

	// Find dominant
	int32 MaxCount = 0;
	EAgentState Dominant = EAgentState::HOLDING;
	for (int32 s = 0; s < 10; ++s)
	{
		if (StateCounts[s] > MaxCount)
		{
			MaxCount = StateCounts[s];
			Dominant = static_cast<EAgentState>(s);
		}
	}

	switch (Dominant)
	{
	case EAgentState::HOLDING:   return TEXT("Stoi");
	case EAgentState::LOADING:   return TEXT("Laduje");
	case EAgentState::AIMING:    return TEXT("Celuje");
	case EAgentState::FIRING:    return TEXT("Strzela!");
	case EAgentState::ADVANCING: return TEXT("Maszeruje");
	case EAgentState::ROUTING:   return TEXT("PANIKA!");
	case EAgentState::RALLYING:  return TEXT("Zbiera sie");
	default:                     return TEXT("---");
	}
}

void ABattleSpawnerActor::SetVolleyModeRuntime(EVolleyMode NewMode)
{
	VolleyMode = NewMode;

	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		FAgentCombatFragment& CF = EM.GetFragmentDataChecked<FAgentCombatFragment>(Entity);
		CF.VolleyMode    = NewMode;
		CF.bVolleyReady  = false;
		CF.bVolleySignal = false;
	}
}

void ABattleSpawnerActor::SetForceRun(bool bRun)
{
	bForceRun = bRun;

	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	// Bieg = bForceRun=true (RunSpeed); Marsz = bForceRun=false (MarchSpeed)
	// Catch-up logic still kicks in automatically when soldier is far from slot.
	const float FormationPace = bRun ? RunSpeed : MarchSpeed;

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		FAgentVelocityFragment& VF = EM.GetFragmentDataChecked<FAgentVelocityFragment>(Entity);
		VF.bForceRun = bRun;
	}

	// Sync officer to formation pace — he leads the line, not sprints ahead.
	if (OfficerEntity.IsValid() && EM.IsEntityValid(OfficerEntity))
	{
		FOfficerFragment& OFF = EM.GetFragmentDataChecked<FOfficerFragment>(OfficerEntity);
		OFF.MoveSpeed = FormationPace;
	}

	// Sync NCOs to formation pace (their ChaseSpeed stays separate for stragglers).
	for (const FMassEntityHandle& NCOHandle : NCOEntities)
	{
		if (!EM.IsEntityValid(NCOHandle)) continue;
		FNCOFragment& NCO = EM.GetFragmentDataChecked<FNCOFragment>(NCOHandle);
		NCO.MoveSpeed = FormationPace;
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// Engagement system (Total War–style attack-move)
// ═══════════════════════════════════════════════════════════════════════════════

void ABattleSpawnerActor::IssueEngageOrder(ABattleSpawnerActor* EnemySquad)
{
	if (!EnemySquad || EnemySquad == this) return;

	EngagedTarget = EnemySquad;
	LastEngageEnemyPos = EnemySquad->GetFormationCenter();

	// Compute approach position: stop at 75% of fire range from enemy
	const FVector MyCenter    = GetFormationCenter();
	const FVector EnemyCenter = LastEngageEnemyPos;
	const FVector ToEnemy     = (EnemyCenter - MyCenter).GetSafeNormal2D();
	const float   StopDist    = SoldierFireRange * 0.75f;
	const FVector EngagePos   = EnemyCenter - ToEnemy * StopDist;

	// Issue full move order (with propagation) — but DON'T clear engagement
	// (IssueMoveOrder clears it, so we do the formation logic inline)
	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	// ── Row size ──────────────────────────────────────────────────────────────
	int32 LocalRowSize = (CurrentRowSize > 0) ? CurrentRowSize
		: FMath::Max(3, FMath::CeilToInt(FMath::Sqrt((float)NumAgents)));
	CurrentRowSize = LocalRowSize;

	const int32 NumRows   = FMath::Max(1, FMath::CeilToInt((float)NumAgents / LocalRowSize));
	const float HalfFront = LocalRowSize * SpawnSpacing * 0.5f;
	const float HalfDepth = NumRows * SpawnSpacing * 0.5f;

	// Front line perpendicular to march direction (toward enemy)
	const FVector FrontLineDir = FVector(ToEnemy.Y, -ToEnemy.X, 0.f);
	const FQuat   FormRot      = FRotationMatrix::MakeFromX(FrontLineDir).ToQuat();

	// ── Assign soldiers ───────────────────────────────────────────────────────
	const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());
	for (int32 i = 0; i < SoldierCount; ++i)
	{
		const FMassEntityHandle Entity = SpawnedEntities[i];
		if (!EM.IsEntityValid(Entity)) continue;

		const int32 Col = i % LocalRowSize;
		const int32 Row = i / LocalRowSize;
		const FVector LocalOffset(
			Col * SpawnSpacing - HalfFront,
			HalfDepth - Row * SpawnSpacing,
			0.f
		);
		const FVector SlotPos = EngagePos + FormRot.RotateVector(LocalOffset);

		FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
		OF.TargetPosition  = SlotPos;
		OF.bHasTarget      = true;
		OF.FaceTarget      = EnemyCenter;
		OF.bHasFaceTarget  = true;

		FOrderPropagationFragment& PF = EM.GetFragmentDataChecked<FOrderPropagationFragment>(Entity);
		PF.bOrderReceived = false;
		PF.bOrderExecuted = false;
		PF.bOrderIgnored  = false;
		PF.ExecutionTimer = 0.f;
		PF.ExecutionDelay = 0.f;

		FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State != EAgentState::DEAD)
		{
			SF.State      = EAgentState::HOLDING;
			SF.StateTimer = 0.f;
		}
	}

	// ── Officer ───────────────────────────────────────────────────────────────
	if (OfficerEntity.IsValid() && EM.IsEntityValid(OfficerEntity))
	{
		FOfficerFragment& OFF = EM.GetFragmentDataChecked<FOfficerFragment>(OfficerEntity);
		const FVector FrontLocal(0.f, HalfDepth + 100.f, 0.f);
		OFF.FormationFrontPos   = EngagePos + FormRot.RotateVector(FrontLocal);
		OFF.bHasFormationTarget = true;
	}

	// ── NCOs ──────────────────────────────────────────────────────────────────
	for (int32 n = 0; n < NCOEntities.Num(); ++n)
	{
		if (!EM.IsEntityValid(NCOEntities[n])) continue;
		FNCOFragment& NCO = EM.GetFragmentDataChecked<FNCOFragment>(NCOEntities[n]);
		const float LateralOffset = (n - (NCOEntities.Num() - 1) * 0.5f) * SpawnSpacing * 2.f;
		const FVector RearLocal(LateralOffset, -(HalfDepth + 200.f), 0.f);
		NCO.FormationPos     = EngagePos + FormRot.RotateVector(RearLocal);
		NCO.bHasFormationPos = true;
		NCO.bHasTarget       = false;
	}

	UE_LOG(LogTemp, Log, TEXT("BattleSpawner squad=%d: ENGAGE enemy squad (team=%d)"),
		MySquadId, EnemySquad->TeamId);
}

void ABattleSpawnerActor::UpdateEngagement()
{
	if (!EngagedTarget) return;

	// Enemy wiped out → disengage
	if (!EngagedTarget->HasAliveSoldiers())
	{
		EngagedTarget = nullptr;
		ClearFaceTarget();
		return;
	}

	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	const FVector MyCenter    = GetFormationCenter();
	const FVector EnemyCenter = EngagedTarget->GetFormationCenter();
	const float   Distance    = (EnemyCenter - MyCenter).Size2D();

	// ── Always update face target so soldiers track the enemy ─────────────
	SetFaceTargetOnSoldiers(EnemyCenter);

	// ── Check if we need to reposition ────────────────────────────────────
	const float EnemyMoved = (EnemyCenter - LastEngageEnemyPos).Size2D();

	// Conditions to re-issue move:
	// 1) Enemy moved significantly (>300cm)
	// 2) AND either we're out of range OR enemy drifted a lot
	const bool bOutOfRange = Distance > SoldierFireRange * 0.9f;
	const bool bEnemyShifted = EnemyMoved > 300.f;

	if (bOutOfRange || (bEnemyShifted && EnemyMoved > 600.f))
	{
		LastEngageEnemyPos = EnemyCenter;

		// Lightweight re-order: just slide target positions, no propagation reset
		const FVector ToEnemy     = (EnemyCenter - MyCenter).GetSafeNormal2D();
		const float   StopDist   = SoldierFireRange * 0.75f;
		const FVector EngagePos  = EnemyCenter - ToEnemy * StopDist;
		const FVector FrontLineDir = FVector(ToEnemy.Y, -ToEnemy.X, 0.f);
		const FQuat   FormRot    = FRotationMatrix::MakeFromX(FrontLineDir).ToQuat();

		const int32 LocalRowSize = (CurrentRowSize > 0) ? CurrentRowSize
			: FMath::Max(3, FMath::CeilToInt(FMath::Sqrt((float)NumAgents)));
		const int32 NumRows   = FMath::Max(1, FMath::CeilToInt((float)NumAgents / LocalRowSize));
		const float HalfFront = LocalRowSize * SpawnSpacing * 0.5f;
		const float HalfDepth = NumRows * SpawnSpacing * 0.5f;

		const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());
		for (int32 i = 0; i < SoldierCount; ++i)
		{
			const FMassEntityHandle Entity = SpawnedEntities[i];
			if (!EM.IsEntityValid(Entity)) continue;

			const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
			if (SF.State == EAgentState::DEAD) continue;

			const int32 Col = i % LocalRowSize;
			const int32 Row = i / LocalRowSize;
			const FVector LocalOffset(
				Col * SpawnSpacing - HalfFront,
				HalfDepth - Row * SpawnSpacing,
				0.f
			);

			FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
			OF.TargetPosition = EngagePos + FormRot.RotateVector(LocalOffset);
			OF.bHasTarget     = true;

			// If out of range, make soldier advance again
			if (bOutOfRange)
			{
				FAgentStateFragment& SFMut = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
				if (SFMut.State == EAgentState::HOLDING || SFMut.State == EAgentState::LOADING)
				{
					SFMut.State      = EAgentState::ADVANCING;
					SFMut.StateTimer = 0.f;
				}
			}
		}

		// Update officer + NCO positions
		if (OfficerEntity.IsValid() && EM.IsEntityValid(OfficerEntity))
		{
			FOfficerFragment& OFF = EM.GetFragmentDataChecked<FOfficerFragment>(OfficerEntity);
			const FVector FrontLocal(0.f, HalfDepth + 100.f, 0.f);
			OFF.FormationFrontPos   = EngagePos + FormRot.RotateVector(FrontLocal);
			OFF.bHasFormationTarget = true;
		}

		for (int32 n = 0; n < NCOEntities.Num(); ++n)
		{
			if (!EM.IsEntityValid(NCOEntities[n])) continue;
			FNCOFragment& NCO = EM.GetFragmentDataChecked<FNCOFragment>(NCOEntities[n]);
			const float LateralOff = (n - (NCOEntities.Num() - 1) * 0.5f) * SpawnSpacing * 2.f;
			const FVector RearLocal(LateralOff, -(HalfDepth + 200.f), 0.f);
			NCO.FormationPos     = EngagePos + FormRot.RotateVector(RearLocal);
			NCO.bHasFormationPos = true;
		}
	}

	// ── Debug: red line from our center to enemy center ───────────────────
	DrawDebugLine(World,
		MyCenter + FVector(0.f, 0.f, 50.f),
		EnemyCenter + FVector(0.f, 0.f, 50.f),
		FColor::Red, false, -1.f, 0, 3.f);
}

void ABattleSpawnerActor::SetFaceTargetOnSoldiers(const FVector& WorldTarget)
{
	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
		OF.FaceTarget     = WorldTarget;
		OF.bHasFaceTarget = true;
	}
}

void ABattleSpawnerActor::ClearFaceTarget()
{
	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
		OF.bHasFaceTarget = false;
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// Volley coordination
// ═══════════════════════════════════════════════════════════════════════════════

void ABattleSpawnerActor::UpdateVolley()
{
	if (VolleyMode == EVolleyMode::FreeFire) return;

	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	// ── Count how many soldiers are alive, ready, and per-row ─────────────
	const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());
	int32 AliveTotal   = 0;
	int32 ReadyTotal   = 0;

	// For RankFire: count per-row
	const int32 LocalRowSize = (CurrentRowSize > 0) ? CurrentRowSize
		: FMath::Max(3, FMath::CeilToInt(FMath::Sqrt((float)NumAgents)));
	const int32 MaxRows = FMath::Max(1, FMath::CeilToInt((float)NumAgents / LocalRowSize));

	TArray<int32> AlivePerRow;
	TArray<int32> ReadyPerRow;
	AlivePerRow.SetNumZeroed(MaxRows);
	ReadyPerRow.SetNumZeroed(MaxRows);

	for (int32 i = 0; i < SoldierCount; ++i)
	{
		const FMassEntityHandle Entity = SpawnedEntities[i];
		if (!EM.IsEntityValid(Entity)) continue;

		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State == EAgentState::DEAD || SF.State == EAgentState::ROUTING) continue;

		const FAgentVelocityFragment& VF = EM.GetFragmentDataChecked<FAgentVelocityFragment>(Entity);
		const FAgentCombatFragment& CF   = EM.GetFragmentDataChecked<FAgentCombatFragment>(Entity);
		const int32 Row = FMath::Clamp(VF.FormationRow, 0, MaxRows - 1);

		++AliveTotal;
		++AlivePerRow[Row];

		if (CF.bVolleyReady)
		{
			++ReadyTotal;
			++ReadyPerRow[Row];
		}
	}

	if (AliveTotal == 0) return;

	// ── Determine if volley should fire ───────────────────────────────────
	bool bFireAll   = false;
	int32 FireRow   = -1;   // -1 = no specific row
	const float ReadyThreshold = 0.75f;

	if (VolleyMode == EVolleyMode::SquadVolley)
	{
		if (ReadyTotal >= FMath::CeilToInt(AliveTotal * ReadyThreshold))
			bFireAll = true;
	}
	else if (VolleyMode == EVolleyMode::RankFire)
	{
		// Find the current rank to fire (cycle through rows)
		for (int32 attempt = 0; attempt < MaxRows; ++attempt)
		{
			const int32 Row = (CurrentVolleyRank + attempt) % MaxRows;
			if (AlivePerRow[Row] == 0) continue;

			if (ReadyPerRow[Row] >= FMath::CeilToInt(AlivePerRow[Row] * ReadyThreshold))
			{
				FireRow = Row;
				CurrentVolleyRank = (Row + 1) % MaxRows;
				break;
			}
			break;  // only check the current rank (wait for it)
		}
	}

	// ── Set volley signal ─────────────────────────────────────────────────
	if (bFireAll || FireRow >= 0)
	{
		for (int32 i = 0; i < SoldierCount; ++i)
		{
			const FMassEntityHandle Entity = SpawnedEntities[i];
			if (!EM.IsEntityValid(Entity)) continue;

			const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
			if (SF.State == EAgentState::DEAD) continue;

			FAgentCombatFragment& CF = EM.GetFragmentDataChecked<FAgentCombatFragment>(Entity);

			if (bFireAll && CF.bVolleyReady)
			{
				CF.bVolleySignal = true;
			}
			else if (FireRow >= 0)
			{
				const FAgentVelocityFragment& VF = EM.GetFragmentDataChecked<FAgentVelocityFragment>(Entity);
				if (VF.FormationRow == FireRow && CF.bVolleyReady)
					CF.bVolleySignal = true;
			}
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// Visualization (ISM)
// ═══════════════════════════════════════════════════════════════════════════════

UStaticMesh* ABattleSpawnerActor::GetFallbackMesh() const
{
	static TObjectPtr<UStaticMesh> CachedMesh = nullptr;
	if (!CachedMesh)
	{
		CachedMesh = LoadObject<UStaticMesh>(nullptr,
			TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	}
	return CachedMesh;
}

UHierarchicalInstancedStaticMeshComponent* ABattleSpawnerActor::CreateHISM(
	FName Name, UStaticMesh* Mesh, UMaterialInterface* Material)
{
	auto* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, Name);
	HISM->SetupAttachment(RootComponent);
	HISM->SetAbsolute(true, true, true);   // world-space instances
	HISM->SetCastShadow(true);
	HISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	if (Mesh)
		HISM->SetStaticMesh(Mesh);

	if (Material)
		HISM->SetMaterial(0, Material);

	HISM->RegisterComponent();
	return HISM;
}

void ABattleSpawnerActor::SetupVisualization()
{
	UStaticMesh* Fallback  = GetFallbackMesh();
	UStaticMesh* SolMesh   = SoldierMesh   ? SoldierMesh.Get()   : Fallback;
	UStaticMesh* OffMesh   = OfficerMesh   ? OfficerMesh.Get()   : SolMesh;
	UStaticMesh* NcoMesh   = NCOMesh       ? NCOMesh.Get()       : SolMesh;

	UMaterialInterface* NcoMat = NCOMaterial ? NCOMaterial.Get() : SoldierMaterial.Get();

	SoldierHISM     = CreateHISM(TEXT("SoldierHISM"),     SolMesh, SoldierMaterial);
	DeadSoldierHISM = CreateHISM(TEXT("DeadSoldierHISM"), SolMesh, DeadSoldierMaterial);
	OfficerHISM     = CreateHISM(TEXT("OfficerHISM"),     OffMesh, SoldierMaterial);
	NCOHISM         = CreateHISM(TEXT("NCOHISM"),         NcoMesh, NcoMat);
}

void ABattleSpawnerActor::UpdateVisualization()
{
	UWorld* World = GetWorld();
	if (!World) return;

	const UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	const FMassEntityManager& EM = Subsystem->GetEntityManager();

	// ── Helper: build instance transform from entity ──────────────────────
	// UE5 cylinder: radius=50, height=100 centered at origin
	// Human scale:  ~170cm tall, ~30cm wide
	// Alive:  scale (0.3, 0.3, 1.7), offset Z +85 (base at feet)
	// Dead:   roll 90°, near ground
	constexpr float AliveScaleXY = 0.3f;
	constexpr float AliveScaleZ  = 1.7f;
	constexpr float AliveZOff    = 85.f;    // half of 170

	// ── Soldiers ──────────────────────────────────────────────────────────
	if (SoldierHISM && DeadSoldierHISM)
	{
		SoldierHISM->ClearInstances();
		DeadSoldierHISM->ClearInstances();

		const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());
		for (int32 i = 0; i < SoldierCount; ++i)
		{
			const FMassEntityHandle Entity = SpawnedEntities[i];
			if (!EM.IsEntityValid(Entity)) continue;

			const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
			const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);

			if (SF.State == EAgentState::DEAD)
			{
				// Fallen: rotate 90° roll, near ground
				FVector Pos = TF.GetTransform().GetLocation();
				Pos.Z = 15.f;
				const float Yaw = TF.GetTransform().Rotator().Yaw;
				FTransform DeadT(FRotator(0.f, Yaw, 90.f).Quaternion(), Pos,
					FVector(AliveScaleXY, AliveScaleXY, AliveScaleZ));
				DeadSoldierHISM->AddInstance(DeadT, /*bWorldSpace=*/true);
			}
			else
			{
				FVector Pos = TF.GetTransform().GetLocation();
				Pos.Z += AliveZOff;
				FTransform AliveT(TF.GetTransform().GetRotation(), Pos,
					FVector(AliveScaleXY, AliveScaleXY, AliveScaleZ));
				SoldierHISM->AddInstance(AliveT, /*bWorldSpace=*/true);
			}
		}
	}

	// ── Officer ───────────────────────────────────────────────────────────
	if (OfficerHISM && OfficerEntity.IsValid() && EM.IsEntityValid(OfficerEntity))
	{
		OfficerHISM->ClearInstances();

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(OfficerEntity);
		const FOfficerFragment& OF   = EM.GetFragmentDataChecked<FOfficerFragment>(OfficerEntity);

		if (OF.bIsAlive)
		{
			// Officer: taller, wider
			FVector Pos = TF.GetTransform().GetLocation();
			Pos.Z += 95.f;
			FTransform OT(TF.GetTransform().GetRotation(), Pos,
				FVector(0.4f, 0.4f, 1.9f));
			OfficerHISM->AddInstance(OT, true);
		}
	}

	// ── NCOs ──────────────────────────────────────────────────────────────
	if (NCOHISM)
	{
		NCOHISM->ClearInstances();

		for (const FMassEntityHandle& NCOHandle : NCOEntities)
		{
			if (!EM.IsEntityValid(NCOHandle)) continue;

			const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(NCOHandle);
			const FNCOFragment& NCO      = EM.GetFragmentDataChecked<FNCOFragment>(NCOHandle);

			if (NCO.bIsAlive)
			{
				// NCO: slightly taller and wider than officer (~210 cm tall).
				// Z offset must match half-height (scale.Z * 100 / 2) so the
				// capsule stands ON the ground, not buried under it.
				constexpr float NCOScaleXY = 0.45f;
				constexpr float NCOScaleZ  = 2.1f;
				constexpr float NCOZOff    = 105.f;   // half of 210

				FVector Pos = TF.GetTransform().GetLocation();
				Pos.Z += NCOZOff;
				FTransform NT(TF.GetTransform().GetRotation(), Pos,
					FVector(NCOScaleXY, NCOScaleXY, NCOScaleZ));
				NCOHISM->AddInstance(NT, true);
			}
		}
	}

	// ── Fire range arc + facing arrow ─────────────────────────────────────
	if (bShowFireRange)
	{
		DrawFireRangeArc(EM);
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// Fire range arc + facing arrow
// ═══════════════════════════════════════════════════════════════════════════════

void ABattleSpawnerActor::DrawFireRangeArc(const FMassEntityManager& EM)
{
	UWorld* World = GetWorld();
	if (!World) return;

	// ── Gather alive soldiers: center + average facing ────────────────────
	FVector SumPos     = FVector::ZeroVector;
	FVector SumForward = FVector::ZeroVector;
	int32   AliveCount = 0;

	const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());
	for (int32 i = 0; i < SoldierCount; ++i)
	{
		const FMassEntityHandle Entity = SpawnedEntities[i];
		if (!EM.IsEntityValid(Entity)) continue;

		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State == EAgentState::DEAD) continue;

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
		SumPos     += TF.GetTransform().GetLocation();
		SumForward += TF.GetTransform().GetUnitAxis(EAxis::X);
		++AliveCount;
	}

	if (AliveCount == 0) return;

	const FVector Center  = SumPos / static_cast<float>(AliveCount);
	FVector AvgFwd        = (SumForward / static_cast<float>(AliveCount)).GetSafeNormal2D();
	if (AvgFwd.IsNearlyZero())
		AvgFwd = GetActorForwardVector().GetSafeNormal2D();

	// Draw height slightly above ground so it doesn't Z-fight
	const FVector DrawBase = FVector(Center.X, Center.Y, 10.f);

	const FColor MyColor  = (TeamId == 0) ? FColor(50, 150, 255) : FColor(255, 80, 80);
	const FColor ArcColor = FColor(MyColor.R, MyColor.G, MyColor.B, 180);

	// ── 1. Facing arrow (thick, 300cm long) ──────────────────────────────
	const float ArrowLen = 400.f;
	const FVector ArrowEnd = DrawBase + AvgFwd * ArrowLen;
	DrawDebugDirectionalArrow(World, DrawBase, ArrowEnd,
		50.f, MyColor, false, -1.f, 0, 5.f);

	// ── 2. Fire cone: two edge lines + arc ───────────────────────────────
	const float HalfAngleRad = FMath::DegreesToRadians(VisionHalfAngleDeg);
	const float Range        = SoldierFireRange;

	// Edge directions (rotate AvgFwd by ±HalfAngle around Z)
	auto RotateZ = [](const FVector& V, float AngleRad) -> FVector
	{
		const float C = FMath::Cos(AngleRad);
		const float S = FMath::Sin(AngleRad);
		return FVector(V.X * C - V.Y * S, V.X * S + V.Y * C, 0.f);
	};

	const FVector LeftDir  = RotateZ(AvgFwd,  HalfAngleRad);
	const FVector RightDir = RotateZ(AvgFwd, -HalfAngleRad);

	const FVector LeftEnd  = DrawBase + LeftDir  * Range;
	const FVector RightEnd = DrawBase + RightDir * Range;

	// Edge lines (thinner than arrow)
	DrawDebugLine(World, DrawBase, LeftEnd,  ArcColor, false, -1.f, 0, 2.5f);
	DrawDebugLine(World, DrawBase, RightEnd, ArcColor, false, -1.f, 0, 2.5f);

	// Arc: segmented curve from RightDir to LeftDir at Range distance
	constexpr int32 ArcSegments = 24;
	const float AngleStep = (2.f * HalfAngleRad) / static_cast<float>(ArcSegments);

	FVector PrevPoint = RightEnd;
	for (int32 s = 1; s <= ArcSegments; ++s)
	{
		const float Angle = -HalfAngleRad + AngleStep * s;
		const FVector Dir = RotateZ(AvgFwd, Angle);
		const FVector Point = DrawBase + Dir * Range;

		DrawDebugLine(World, PrevPoint, Point, ArcColor, false, -1.f, 0, 2.5f);
		PrevPoint = Point;
	}

	// ── 3. Inner arc at 60% range (effective zone) ───────────────────────
	const float InnerRange = Range * 0.6f;
	const FColor InnerColor = FColor(MyColor.R, MyColor.G, MyColor.B, 100);

	FVector PrevInner = DrawBase + RightDir * InnerRange;
	for (int32 s = 1; s <= ArcSegments; ++s)
	{
		const float Angle = -HalfAngleRad + AngleStep * s;
		const FVector Dir = RotateZ(AvgFwd, Angle);
		const FVector Point = DrawBase + Dir * InnerRange;

		DrawDebugLine(World, PrevInner, Point, InnerColor, false, -1.f, 0, 1.5f);
		PrevInner = Point;
	}
}
