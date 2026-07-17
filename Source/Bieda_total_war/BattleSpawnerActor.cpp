#include "BattleSpawnerActor.h"
#include "MassEntitySubsystem.h"
#include "MassEntityManager.h"
#include "MassCommonFragments.h"
#include "BattleTypes.h"
#include "BattleDebugProcessor.h"   // BiedaDebugDrawEnabled()
#include "BattleSimControl.h"       // BattleSimPaused()
#include "BattleStats.h"
#include "DrawDebugHelpers.h"
#include "BattleSoldierCharacter.h"
#include "BattleCameraPawn.h"
#include "GameFramework/PlayerController.h"

DECLARE_CYCLE_STAT(TEXT("Spawner: Visualization"), STAT_BiedaVis,    STATGROUP_Bieda);
DECLARE_CYCLE_STAT(TEXT("Spawner: Engagement"),    STAT_BiedaEngage, STATGROUP_Bieda);
DECLARE_CYCLE_STAT(TEXT("Spawner: Volley"),        STAT_BiedaVolley, STATGROUP_Bieda);
DECLARE_CYCLE_STAT(TEXT("Spawner: Stragglers"),    STAT_BiedaStrag,  STATGROUP_Bieda);
DECLARE_CYCLE_STAT(TEXT("Spawner: CasualtyShock"), STAT_BiedaShock,  STATGROUP_Bieda);

namespace { uint8 GNextSquadId = 0; }

ABattleSpawnerActor::ABattleSpawnerActor()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	// NOTE: Soldier_20K (from Infantry20K.fbx) is a SKELETAL mesh (imported with
	// its rig intact) — HISM can only render STATIC meshes, so it can't be used
	// here. Soldier_Retopo is the same retopologized model line but imported as
	// a plain static mesh (40k tris, no bones).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SoldierMeshFinder(
		TEXT("/Game/Models/Soldier_Retopo.Soldier_Retopo"));
	if (SoldierMeshFinder.Succeeded())
	{
		SoldierMesh = SoldierMeshFinder.Object;
	}
}

void ABattleSpawnerActor::BeginPlay()
{
	Super::BeginPlay();
	SetupVisualization();
	SpawnAgents();
}

void ABattleSpawnerActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyAnimatedActors();
	Super::EndPlay(EndPlayReason);
}

bool ABattleSpawnerActor::ShouldUseAnimatedActors() const
{
	if (AnimatedActorRange <= 0.f) return false;

	UWorld* World = GetWorld();
	if (!World) return false;

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC) return false;

	ABattleCameraPawn* Cam = Cast<ABattleCameraPawn>(PC->GetPawn());
	if (!Cam) return false;

	if (Cam->GetSelectedSpawner() == this) return true;

	const float DistSq = FVector::DistSquared(Cam->GetActorLocation(), GetFormationCenter());
	return DistSq < FMath::Square(AnimatedActorRange);
}

void ABattleSpawnerActor::EnsureAnimatedActorsSpawned()
{
	if (AnimatedActors.Num() > 0) return;

	UWorld* World = GetWorld();
	if (!World) return;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Owner = this;

	AnimatedActors.Reserve(NumAgents);
	for (int32 i = 0; i < NumAgents; ++i)
	{
		if (ABattleSoldierCharacter* Actor = World->SpawnActor<ABattleSoldierCharacter>(Params))
			AnimatedActors.Add(Actor);
	}
}

void ABattleSpawnerActor::DestroyAnimatedActors()
{
	for (ABattleSoldierCharacter* Actor : AnimatedActors)
		if (Actor && IsValid(Actor)) Actor->Destroy();
	AnimatedActors.Empty();
}

void ABattleSpawnerActor::SyncAnimatedActors(const FMassEntityManager& EM)
{
	const int32 Count = FMath::Min(AnimatedActors.Num(), SpawnedEntities.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		ABattleSoldierCharacter* Actor = AnimatedActors[i];
		if (!Actor) continue;

		const FMassEntityHandle Entity = SpawnedEntities[i];
		if (!EM.IsEntityValid(Entity))
		{
			Actor->SetActorHiddenInGame(true);
			continue;
		}

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);

		Actor->SetActorHiddenInGame(false);
		Actor->SetActorLocationAndRotation(TF.GetTransform().GetLocation(), TF.GetTransform().GetRotation());
		Actor->SetSoldierState(SF.State);
	}
}

void ABattleSpawnerActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Check for debug-capsule toggle at runtime (console var or property change)
	{
		const bool bDesired = DebugCapsulesDesired();
		if (bDesired != bLastDebugState)
		{
			bLastDebugState = bDesired;
			SetupVisualization();
		}
	}

	// Simulation helpers freeze on tactical pause; visualization keeps drawing
	// the frozen state so the player can pan around and issue orders. Their
	// time-based logic (shock decay, volley gap) scales with game speed too.
	if (!BattleSimPaused())
	{
		const float SimDelta = DeltaSeconds * BattleSimTimeScale();
		UpdateEngagement();
		UpdateVolley(SimDelta);
		UpdateStragglers();
		UpdateCasualtyShock(SimDelta);
	}
	UpdateVisualization();
}

void ABattleSpawnerActor::UpdateStragglers()
{
	SCOPE_CYCLE_COUNTER(STAT_BiedaStrag);
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

ABattleSpawnerActor::FFormationDims ABattleSpawnerActor::ComputeFormationDims(int32 InRowSize) const
{
	// Men in a rank stand shoulder-to-shoulder — tighter side-to-side than the
	// gap between ranks. ColSpacing (within a row) is half SpawnSpacing; rank
	// depth keeps the full SpawnSpacing.
	FFormationDims D;
	D.Cols      = FMath::Max(1, InRowSize);
	D.Rows      = FMath::Max(1, FMath::CeilToInt((float)NumAgents / D.Cols));
	D.HalfFront = D.Cols * ColSpacing() * 0.5f;
	D.HalfDepth = D.Rows * SpawnSpacing * 0.5f;
	return D;
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
		FFactionFragment::StaticStruct(),
		FFatigueFragment::StaticStruct()
	};
	FMassArchetypeHandle Archetype = EM.CreateArchetype(Fragments);

	// Line infantry fights by DISCIPLINED VOLLEY by default — the whole company
	// holds, then fires together (both ranks at once), then reloads together.
	// Militia stays on FreeFire (loose, individual, ragged). The designer can
	// still override VolleyMode in the editor before play; we only auto-promote
	// the FreeFire default, never stomp an explicit SquadVolley/RankFire choice.
	if (UnitType == EUnitType::LineInfantry && VolleyMode == EVolleyMode::FreeFire)
	{
		VolleyMode = EVolleyMode::SquadVolley;
	}

	const FVector Base = GetActorLocation();

	// RowSize: 0 = auto, otherwise use value from editor.
	//   Line Infantry + bTwoRankLine → 2-rank-deep line (British "thin red line").
	//   Otherwise → square-ish grid.
	int32 EffectiveRowSize;
	if (RowSize > 0)
	{
		EffectiveRowSize = FMath::Clamp(RowSize, 1, NumAgents);
	}
	else if (UnitType == EUnitType::LineInfantry && bTwoRankLine)
	{
		EffectiveRowSize = FMath::Max(1, FMath::CeilToInt(NumAgents / 2.f));
	}
	else
	{
		EffectiveRowSize = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt((float)NumAgents)));
	}

	// Deployment-width cap (future deploy zones): if the rank would be wider than
	// MaxDeployWidth, shrink it and let the overflow wrap into extra rows behind.
	// 0 = no cap (default — nothing changes). Full width = Cols * ColSpacing().
	if (MaxDeployWidth > 0.f && ColSpacing() > 0.f)
	{
		const int32 MaxCols = FMath::Max(1, FMath::FloorToInt(MaxDeployWidth / ColSpacing()));
		EffectiveRowSize = FMath::Min(EffectiveRowSize, MaxCols);
	}

	CurrentRowSize = EffectiveRowSize;   // store for future quick-move orders

	const FFormationDims Dims = ComputeFormationDims(EffectiveRowSize);
	const float HalfFront    = Dims.HalfFront;   // half formation width
	const float HalfDepth    = Dims.HalfDepth;   // half formation depth

	// ── Direction basis (same convention as move/engage/halt) ────────────────
	// Fwd = where soldiers look (toward the enemy), Lat = along the front line.
	// Everything below is built as  Base + Lat*lateral + Fwd*forward  so the
	// formation is ALWAYS perpendicular to the facing — never sideways.
	FVector Fwd = SpawnFacing.Vector().GetSafeNormal2D();
	if (Fwd.IsNearlyZero()) Fwd = FVector(1.f, 0.f, 0.f);
	const FVector Lat(-Fwd.Y, Fwd.X, 0.f);
	const FQuat   SpawnRot = FRotationMatrix::MakeFromX(Fwd).ToQuat();

	SpawnedEntities.Reserve(NumAgents);

	// Mark which rank-and-file are corporals — spread evenly, visual only.
	CorporalFlags.Init(false, NumAgents);
	{
		const int32 NumCorp = FMath::Clamp(NumCorporals, 0, NumAgents);
		for (int32 c = 0; c < NumCorp; ++c)
		{
			const int32 Idx = FMath::Clamp(
				FMath::RoundToInt((c + 0.5f) * NumAgents / NumCorp), 0, NumAgents - 1);
			CorporalFlags[Idx] = true;
		}
	}

	for (int32 i = 0; i < NumAgents; ++i)
	{
		const int32 Row = i / EffectiveRowSize;
		const int32 Col = i % EffectiveRowSize;
		// Lateral spreads along the front line; forward places row 0 at the FRONT
		// (toward Fwd) with later ranks stepping back — same as the dressed move.
		const int32 InRow = FMath::Min(EffectiveRowSize, NumAgents - Row * EffectiveRowSize);
		const float CenterShift = (EffectiveRowSize - InRow) * ColSpacing() * 0.5f;  // centre a partial last row
		const float LatOff = Col * ColSpacing() - HalfFront + CenterShift;
		const float FwdOff = HalfDepth - Row * SpawnSpacing;
		const FVector SpawnPos = Base + Lat * LatOff + Fwd * FwdOff;

		FMassEntityHandle Entity = EM.CreateEntity(Archetype);

		FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
		TF.GetMutableTransform() = FTransform(SpawnRot, SpawnPos, FVector::OneVector);

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

		// Per-soldier personality — disciplined for line infantry, sloppy for militia
		float FinalOffsetMax;   // cm — max magnitude of the per-soldier resting offset
		if (UnitType == EUnitType::LineInfantry)
		{
			VF.PersonalSlotTolerance    = FMath::FRandRange(15.f,  80.f);
			VF.PersonalSnapTime         = FMath::FRandRange( 1.5f,  2.5f);
			VF.PersonalDriftAmp         = FMath::FRandRange( 0.5f,  3.f);
			VF.PersonalWaverAmp         = FMath::FRandRange( 0.005f, 0.03f);
			// Shoulder-to-shoulder: separation MUST be smaller than the column
			// spacing (ColSpacing = SpawnSpacing*0.5, ~60cm) or neighbours shove
			// each other out of the slot and the rank dissolves into a blob.
			VF.PersonalSeparationRadius = FMath::FRandRange(35.f, 50.f);
			FinalOffsetMax              = 25.f;
		}
		else  // Militia
		{
			VF.PersonalSlotTolerance    = FMath::FRandRange(80.f, 200.f);
			VF.PersonalSnapTime         = FMath::FRandRange( 2.f,   4.f);
			VF.PersonalDriftAmp         = FMath::FRandRange(10.f,  30.f);
			VF.PersonalWaverAmp         = FMath::FRandRange( 0.06f, 0.15f);
			VF.PersonalSeparationRadius = FMath::FRandRange(120.f, 180.f);
			FinalOffsetMax              = 60.f;
		}

		// Random 2D offset (uniform within a disc) — soldier's permanent quirk:
		// the spot he ALWAYS ends up at relative to his slot, regardless of how
		// many move orders are issued.
		const float OffsetAngle = FMath::FRandRange(0.f, 2.f * UE_PI);
		const float OffsetMag   = FMath::FRandRange(0.f, FinalOffsetMax);
		VF.PersonalFinalOffset  = FVector(
			OffsetMag * FMath::Cos(OffsetAngle),
			OffsetMag * FMath::Sin(OffsetAngle),
			0.f);

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
		CF.UnitType           = UnitType;
		CF.bVolleyReady       = false;
		CF.bVolleySignal      = false;

		// Per-type combat + nerve. Line infantry are drilled regulars: tighter
		// reload/accuracy spread AND steadier nerve (they break later). Militia
		// are ragged: wider spread AND jumpier (they break sooner). The morale
		// thresholds drive the state machine — ShakenThreshold = start wavering /
		// hold fire, PanicThreshold = full rout.
		if (UnitType == EUnitType::LineInfantry)
		{
			CF.ReloadDuration  = SoldierReloadTime * FMath::RandRange(0.9f, 1.1f);
			CF.Accuracy        = SoldierAccuracy   * FMath::RandRange(0.85f, 1.15f);
			CF.PanicThreshold  = 12.f;   // routs late
			CF.ShakenThreshold = 25.f;   // heavy waver only when badly hurt
			CF.WaverThreshold  = 42.f;   // mild waver under pressure
			CF.ShakenRecover   = 35.f;
			CF.WaverRecover    = 55.f;
		}
		else  // Militia
		{
			CF.ReloadDuration  = SoldierReloadTime * FMath::RandRange(0.8f, 1.2f);
			CF.Accuracy        = SoldierAccuracy   * FMath::RandRange(0.75f, 1.25f);
			CF.PanicThreshold  = 30.f;   // routs sooner
			CF.ShakenThreshold = 20.f;   // severe waver
			CF.WaverThreshold  = 42.f;   // mild waver early
			CF.ShakenRecover   = 30.f;
			CF.WaverRecover    = 52.f;
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

		// Officer stands at the FRONT of the formation (centered, just ahead of the line)
		const FVector OfficerPos = Base + Fwd * (HalfDepth + 100.f);

		FTransformFragment& OTF = EM.GetFragmentDataChecked<FTransformFragment>(OfficerEntity);
		OTF.GetMutableTransform() = FTransform(SpawnRot, OfficerPos, FVector::OneVector);

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

			// File closers: spread across 80% of the front width, just behind the line
			const float LateralOffset = (NumNCOs > 1)
				? (static_cast<float>(n) / (NumNCOs - 1) - 0.5f) * (HalfFront * 2.f) * 0.8f
				: 0.f;
			const FVector NCOPos = Base + Lat * LateralOffset
				- Fwd * (HalfDepth + SpawnSpacing * 0.75f);

			FTransformFragment& NTF = EM.GetFragmentDataChecked<FTransformFragment>(NCOEntity);
			NTF.GetMutableTransform() = FTransform(SpawnRot, NCOPos, FVector::OneVector);

			FMoraleFragment& NMF = EM.GetFragmentDataChecked<FMoraleFragment>(NCOEntity);
			NMF.Morale = OfficerInitialMorale;   // NCOs have officer-level morale

			FNCOFragment& NCO = EM.GetFragmentDataChecked<FNCOFragment>(NCOEntity);
			NCO.bIsAlive        = true;
			NCO.FormationPos    = NCOPos;
			NCO.bHasFormationPos = true;
			NCO.MoveSpeed       = MarchSpeed;     // formation pace (SetForceRun syncs this)
			NCO.ChaseSpeed      = 450.f;          // when chasing stragglers/routing (decoupled from soldier CatchUpSpeed)

			FFactionFragment& NFF = EM.GetFragmentDataChecked<FFactionFragment>(NCOEntity);
			NFF.TeamId  = TeamId;
			NFF.SquadId = MySquadId;

			NCOEntities.Add(NCOEntity);
		}
	}

	// ── Drummers (doboszowie — march with the line, relay orders by drumbeat) ──
	if (NumDrummers > 0)
	{
		TArray<const UScriptStruct*> DrummerFrags = {
			FTransformFragment::StaticStruct(),
			FMoraleFragment::StaticStruct(),
			FDrummerFragment::StaticStruct(),
			FFactionFragment::StaticStruct()
		};
		FMassArchetypeHandle DrummerArchetype = EM.CreateArchetype(DrummerFrags);
		DrummerEntities.Reserve(NumDrummers);

		for (int32 d = 0; d < NumDrummers; ++d)
		{
			FMassEntityHandle DrummerEntity = EM.CreateEntity(DrummerArchetype);

			// Musicians (drummer + fifer) stand BESIDE the officer at the front
			// of the column, just off to one side, and follow him. Officer sits at
			// (0, +HalfDepth+100); place musicians at the same depth, fanned out
			// to the flank so they don't overlap him.
			const float SideStep = 150.f;   // cm out to the side per musician
			const float LateralOffset = (static_cast<float>(d) + 1.f) * SideStep;
			const FVector DrummerPos = Base + Lat * LateralOffset
				+ Fwd * (HalfDepth + 100.f);

			FTransformFragment& DTF = EM.GetFragmentDataChecked<FTransformFragment>(DrummerEntity);
			DTF.GetMutableTransform() = FTransform(SpawnRot, DrummerPos, FVector::OneVector);

			FMoraleFragment& DMF = EM.GetFragmentDataChecked<FMoraleFragment>(DrummerEntity);
			DMF.Morale = OfficerInitialMorale;   // drummers steady, near the colors

			FDrummerFragment& DR = EM.GetFragmentDataChecked<FDrummerFragment>(DrummerEntity);
			DR.bIsAlive         = true;
			DR.MoveSpeed        = MarchSpeed;     // formation pace (SetForceRun syncs this)
			DR.DrumRadius       = DrumVoiceRadius;
			DR.FormationPos     = DrummerPos;
			DR.bHasFormationPos = true;

			FFactionFragment& DFF = EM.GetFragmentDataChecked<FFactionFragment>(DrummerEntity);
			DFF.TeamId  = TeamId;
			DFF.SquadId = MySquadId;

			DrummerEntities.Add(DrummerEntity);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("BattleSpawner: %d soldiers + officer=%d + NCOs=%d + drummers=%d  squad=%d team=%d rowSize=%d"),
		SpawnedEntities.Num(), bSpawnOfficer ? 1 : 0, NCOEntities.Num(), DrummerEntities.Num(),
		MySquadId, TeamId, CurrentRowSize);
}

void ABattleSpawnerActor::IssueMoveOrder(const FVector& NewWorldTarget, int32 InRowSize,
	const FVector& InFrontLineDir, bool bInstant)
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
	else if (UnitType == EUnitType::LineInfantry && bTwoRankLine)
	{
		LocalRowSize = FMath::Max(3, FMath::CeilToInt(NumAgents / 2.f));
		CurrentRowSize = LocalRowSize;
	}
	else
	{
		LocalRowSize = FMath::Max(3, FMath::CeilToInt(FMath::Sqrt((float)NumAgents)));
		CurrentRowSize = LocalRowSize;
	}

	// ── Gather only the LIVING soldiers ───────────────────────────────────────
	// Slots must match the number of men still alive — dead bodies don't hold a
	// place in the formation. Otherwise re-forming (e.g. 2 ranks → 3) leaves gaps
	// where casualties used to be. (Total War–style: ranks close up over losses.)
	TArray<FMassEntityHandle> LiveSoldiers;
	LiveSoldiers.Reserve(SpawnedEntities.Num());
	for (const FMassEntityHandle& E : SpawnedEntities)
	{
		if (!EM.IsEntityValid(E)) continue;
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(E);
		if (SF.State == EAgentState::DEAD) continue;
		LiveSoldiers.Add(E);
	}
	const int32 LiveCount = LiveSoldiers.Num();

	// Formation geometry sized to the LIVING count, not the original roster.
	// HalfFront uses ColSpacing (tight ranks); HalfDepth uses full SpawnSpacing.
	const int32 LiveRows  = FMath::Max(1, FMath::CeilToInt((float)LiveCount / LocalRowSize));
	const int32 NumRows   = LiveRows;
	const float HalfFront = LocalRowSize * ColSpacing() * 0.5f;
	const float HalfDepth = LiveRows     * SpawnSpacing * 0.5f;

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
	// Direction soldiers face = perpendicular to the front line (toward the foe).
	// Used to orient them when teleporting into place during deployment.
	const FVector FaceDir = FormRot.RotateVector(FVector(0.f, 1.f, 0.f));
	const FQuat   FaceRot = FRotationMatrix::MakeFromX(FaceDir).ToQuat();

	// ── Assign each LIVING soldier a formation slot (k = packed live index) ───
	for (int32 k = 0; k < LiveCount; ++k)
	{
		const FMassEntityHandle Entity = LiveSoldiers[k];

		const int32 Col = k % LocalRowSize;
		const int32 Row = k / LocalRowSize;

		// Col = position along front line (X of rotation)
		// Row = depth, front row (Row=0) at +Y, back rows toward -Y
		const int32 InRow = FMath::Min(LocalRowSize, LiveCount - Row * LocalRowSize);
		const float CenterShift = (LocalRowSize - InRow) * ColSpacing() * 0.5f;  // centre partial last row
		const FVector LocalOffset(
			Col * ColSpacing() - HalfFront + CenterShift,
			HalfDepth - Row * SpawnSpacing,
			0.f
		);
		const FVector SlotPos = NewWorldTarget + FormRot.RotateVector(LocalOffset);

		FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
		OF.TargetPosition  = SlotPos;
		OF.bHasTarget      = true;

		// Deploy: teleport the soldier straight into his slot (no marching).
		if (bInstant)
		{
			FTransformFragment& TTF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
			TTF.GetMutableTransform() = FTransform(FaceRot, SlotPos, FVector::OneVector);
		}

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
		if (bInstant)
		{
			FTransformFragment& OTF = EM.GetFragmentDataChecked<FTransformFragment>(OfficerEntity);
			OTF.GetMutableTransform() = FTransform(FaceRot, OFF.FormationFrontPos, FVector::OneVector);
		}
	}

	// ── Update NCO formation positions (file closers behind the line) ─────────
	for (int32 n = 0; n < NCOEntities.Num(); ++n)
	{
		if (!EM.IsEntityValid(NCOEntities[n])) continue;
		FNCOFragment& NCO = EM.GetFragmentDataChecked<FNCOFragment>(NCOEntities[n]);
		const float LateralOffset = (NCOEntities.Num() > 1)
			? (static_cast<float>(n) / (NCOEntities.Num() - 1) - 0.5f) * (HalfFront * 2.f) * 0.8f
			: 0.f;
		const FVector RearLocal(LateralOffset, -(HalfDepth + SpawnSpacing * 0.75f), 0.f);
		NCO.FormationPos     = NewWorldTarget + FormRot.RotateVector(RearLocal);
		NCO.bHasFormationPos = true;
		NCO.bHasTarget       = false;   // clear stale target on new order
		if (bInstant)
		{
			FTransformFragment& NTF = EM.GetFragmentDataChecked<FTransformFragment>(NCOEntities[n]);
			NTF.GetMutableTransform() = FTransform(FaceRot, NCO.FormationPos, FVector::OneVector);
		}
	}

	// ── Update drummer/fifer positions — BESIDE the officer at the front ──────
	for (int32 d = 0; d < DrummerEntities.Num(); ++d)
	{
		if (!EM.IsEntityValid(DrummerEntities[d])) continue;
		FDrummerFragment& DR = EM.GetFragmentDataChecked<FDrummerFragment>(DrummerEntities[d]);
		const float SideStep = 150.f;
		const float LateralOffset = (static_cast<float>(d) + 1.f) * SideStep;
		const FVector FrontLocal(LateralOffset, HalfDepth + 100.f, 0.f);
		DR.FormationPos     = NewWorldTarget + FormRot.RotateVector(FrontLocal);
		DR.bHasFormationPos = true;
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

	// ── Stop drummers ────────────────────────────────────────────────────────
	for (const FMassEntityHandle& Drummer : DrummerEntities)
	{
		if (!EM.IsEntityValid(Drummer)) continue;
		FDrummerFragment& DR = EM.GetFragmentDataChecked<FDrummerFragment>(Drummer);
		DR.bHasFormationPos = false;
	}
}

void ABattleSpawnerActor::IssueHaltOrder()
{
	// "Stać" — halt and dress the ranks where the unit currently stands.
	// The front rank is anchored on the MOST-ADVANCED soldier (rank 1 = frontmost);
	// everyone else dresses forward to him, so the line tightens without the front
	// edge stepping back. The order is delivered through the same officer→drummer→
	// peer propagation wave as a normal move order, so soldiers stop with a natural
	// stagger and bleed off running speed. Type-agnostic: militia dress loosely,
	// line infantry tightly (their per-soldier tolerances do the rest).

	EngagedTarget = nullptr;
	ClearFaceTarget();

	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	// ── Gather living soldiers: handle + position + averaged facing ──────────
	TArray<FMassEntityHandle> Living;
	TArray<FVector>           LivePos;
	Living.Reserve(SpawnedEntities.Num());
	LivePos.Reserve(SpawnedEntities.Num());
	FVector FacingSum = FVector::ZeroVector;

	for (const FMassEntityHandle& E : SpawnedEntities)
	{
		if (!EM.IsEntityValid(E)) continue;
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(E);
		if (SF.State == EAgentState::DEAD) continue;

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(E);
		Living.Add(E);
		LivePos.Add(TF.GetTransform().GetLocation());
		FacingSum += TF.GetTransform().GetUnitAxis(EAxis::X);
	}

	const int32 N = Living.Num();
	if (N == 0) return;

	// ── Forward (facing) + lateral basis ─────────────────────────────────────
	FVector Fwd = FacingSum.GetSafeNormal2D();
	if (Fwd.IsNearlyZero()) Fwd = SpawnFacing.Vector().GetSafeNormal2D();
	if (Fwd.IsNearlyZero()) Fwd = FVector(1.f, 0.f, 0.f);
	const FVector LatDir(-Fwd.Y, Fwd.X, 0.f);   // horizontal, perpendicular to Fwd

	// ── Decompose positions: forward coord, lateral coord; front anchor ──────
	TArray<float> Fcoord; Fcoord.SetNumUninitialized(N);
	TArray<float> Lcoord; Lcoord.SetNumUninitialized(N);
	float FrontF  = -FLT_MAX;
	float LatSum  = 0.f;
	float ZSum    = 0.f;
	for (int32 i = 0; i < N; ++i)
	{
		Fcoord[i] = FVector::DotProduct(LivePos[i], Fwd);
		Lcoord[i] = FVector::DotProduct(LivePos[i], LatDir);
		FrontF    = FMath::Max(FrontF, Fcoord[i]);
		LatSum   += Lcoord[i];
		ZSum     += LivePos[i].Z;
	}
	const float LatCenter = LatSum / N;
	const float BaseZ     = ZSum  / N;

	// ── Column count: keep the formation's current width ─────────────────────
	int32 Cols;
	if (CurrentRowSize > 0)
		Cols = FMath::Clamp(CurrentRowSize, 1, N);
	else if (UnitType == EUnitType::LineInfantry && bTwoRankLine)
		Cols = FMath::Max(1, FMath::CeilToInt(N / 2.f));
	else
		Cols = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt((float)N)));
	const int32 Rows      = FMath::Max(1, FMath::CeilToInt((float)N / Cols));
	const float HalfFront = Cols * ColSpacing() * 0.5f;

	// ── Slot assignment: frontmost soldiers fill the front rank, dress forward.
	// Sort all soldiers front→back, then within each rank sort left→right so men
	// keep their side and don't cross over each other.
	TArray<int32> Order;
	Order.SetNumUninitialized(N);
	for (int32 i = 0; i < N; ++i) Order[i] = i;
	Order.Sort([&Fcoord](int32 A, int32 B) { return Fcoord[A] > Fcoord[B]; });

	TArray<int32> Slot;            // Slot[k] = index into Living for slot k
	Slot.SetNumUninitialized(N);
	for (int32 r = 0; r < Rows; ++r)
	{
		const int32 Start = r * Cols;
		const int32 Count = FMath::Min(Cols, N - Start);

		TArray<int32> RankIdx;
		RankIdx.Reserve(Count);
		for (int32 c = 0; c < Count; ++c) RankIdx.Add(Order[Start + c]);
		RankIdx.Sort([&Lcoord](int32 A, int32 B) { return Lcoord[A] < Lcoord[B]; });

		for (int32 c = 0; c < Count; ++c) Slot[Start + c] = RankIdx[c];
	}

	// ── Apply dressed slots + reset order propagation (re-arm the wave) ──────
	for (int32 k = 0; k < N; ++k)
	{
		const int32 Row = k / Cols;
		const int32 Col = k % Cols;
		const FMassEntityHandle E = Living[Slot[k]];

		const float SlotF = FrontF - Row * SpawnSpacing;
		const int32 InRow = FMath::Min(Cols, N - Row * Cols);
		const float SlotL = LatCenter + (Col - (InRow - 1) * 0.5f) * ColSpacing();  // centre partial last row
		const FVector SlotPos = Fwd * SlotF + LatDir * SlotL + FVector(0.f, 0.f, BaseZ);

		FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(E);
		OF.TargetPosition  = SlotPos;
		OF.bHasTarget      = true;

		FAgentVelocityFragment& VF = EM.GetFragmentDataChecked<FAgentVelocityFragment>(E);
		VF.FormationRow = Row;
		VF.FormationCol = Col;
		VF.bForceRun    = false;   // settle at march pace — bleed off the run
		{
			const float ColNorm = (Cols > 1)
				? static_cast<float>(Col) / static_cast<float>(Cols - 1) : 0.5f;
			VF.CurveOffset = FormationCurveStrength * FMath::Sin(ColNorm * UE_PI)
				+ FormationCurveStrength * 0.3f
				  * FMath::Sin(ColNorm * UE_PI * 2.f + VF.NoiseSeed * 6.28f);
		}

		FOrderPropagationFragment& PF = EM.GetFragmentDataChecked<FOrderPropagationFragment>(E);
		PF.bOrderReceived = false;
		PF.bOrderExecuted = false;
		PF.bOrderIgnored  = false;
		PF.ExecutionTimer = 0.f;
		PF.ExecutionDelay = 0.f;

		FAgentCombatFragment& CF = EM.GetFragmentDataChecked<FAgentCombatFragment>(E);
		CF.bVolleyReady  = false;
		CF.bVolleySignal = false;

		FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(E);
		if (SF.State != EAgentState::DEAD)
		{
			SF.State      = EAgentState::HOLDING;
			SF.StateTimer = 0.f;
		}
	}

	bForceRun = false;   // unit is at rest → Marsz footing

	// ── Re-anchor officer / NCOs / drummers to the dressed formation ─────────
	const float RearF = FrontF - (Rows - 1) * SpawnSpacing;   // last-rank depth

	if (OfficerEntity.IsValid() && EM.IsEntityValid(OfficerEntity))
	{
		FOfficerFragment& OFF = EM.GetFragmentDataChecked<FOfficerFragment>(OfficerEntity);
		OFF.FormationFrontPos   = Fwd * (FrontF + 100.f) + LatDir * LatCenter + FVector(0.f, 0.f, BaseZ);
		OFF.bHasFormationTarget = true;
		OFF.MoveSpeed           = MarchSpeed;
	}

	for (int32 n = 0; n < NCOEntities.Num(); ++n)
	{
		if (!EM.IsEntityValid(NCOEntities[n])) continue;
		FNCOFragment& NCO = EM.GetFragmentDataChecked<FNCOFragment>(NCOEntities[n]);
		const float Spread = (NCOEntities.Num() > 1)
			? (static_cast<float>(n) / (NCOEntities.Num() - 1) - 0.5f) * (HalfFront * 2.f) * 0.8f
			: 0.f;
		NCO.FormationPos     = Fwd * (RearF - SpawnSpacing * 0.75f)
			+ LatDir * (LatCenter + Spread) + FVector(0.f, 0.f, BaseZ);
		NCO.bHasFormationPos = true;
		NCO.bHasTarget       = false;
		NCO.MoveSpeed        = MarchSpeed;
	}

	for (int32 d = 0; d < DrummerEntities.Num(); ++d)
	{
		if (!EM.IsEntityValid(DrummerEntities[d])) continue;
		FDrummerFragment& DR = EM.GetFragmentDataChecked<FDrummerFragment>(DrummerEntities[d]);
		// Beside the officer at the front, fanned to one flank.
		const float SideStep = 150.f;
		const float Side = (static_cast<float>(d) + 1.f) * SideStep;
		DR.FormationPos     = Fwd * (FrontF + 100.f)
			+ LatDir * (LatCenter + Side) + FVector(0.f, 0.f, BaseZ);
		DR.bHasFormationPos = true;
		DR.MoveSpeed        = MarchSpeed;
	}

	UE_LOG(LogTemp, Log, TEXT("BattleSpawner squad=%d: STAĆ — dress in place, cols=%d rows=%d N=%d"),
		MySquadId, Cols, Rows, N);
}

void ABattleSpawnerActor::UpdateCasualtyShock(float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_BiedaShock);
	// Two coupled morale-collapse mechanisms, both emergent (no new state):
	//   1) CASUALTY SHOCK — each death drops a local, time-decaying morale-hit
	//      emitter where the soldier fell. Survivors nearby bleed morale by
	//      distance falloff. A salvo stacks many emitters at once → the men by
	//      the gap rout, and panic ripples back rank-by-rank via the existing
	//      contagion. Trickle deaths decay before stacking → little effect.
	//   2) ATTRITION CEILING — caps every soldier's morale at a value that
	//      falls quadratically with losses. Near ~70% losses the ceiling sinks
	//      under the rout threshold → the unit breaks "around 30% strength".
	if (DeltaSeconds <= 0.f) return;

	UWorld* World = GetWorld();
	if (!World) return;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();

	const int32 Count = SpawnedEntities.Num();
	if (Count == 0) return;

	// Lazy init of bookkeeping arrays (first tick after spawn).
	if (SoldierWasAlive.Num() != Count)
	{
		SoldierWasAlive.Init(true, Count);
		InitialSoldierCount = FMath::Max(1, GetAliveCount());
	}

	// Strength of a fresh death's shock scales with how chewed-up the squad
	// already is (loss ratio BEFORE this frame's deaths). A death in a near-full
	// company barely registers; a death in a bled-white remnant is terrifying —
	// so the unit grows more panic-prone the smaller it gets. Scale-independent:
	// works the same for 25, 50 or 150 men because it's a ratio. Floor 0.15 so a
	// death is never literally free.
	int32 AliveBefore = 0;
	for (int32 i = 0; i < Count; ++i)
		if (SoldierWasAlive[i]) ++AliveBefore;
	const float LossBefore = 1.f - (static_cast<float>(AliveBefore) /
		static_cast<float>(InitialSoldierCount));
	const float ShockLossScale = 0.15f + 0.85f * FMath::Clamp(LossBefore, 0.f, 1.f);

	// ── Pass 1: detect new deaths → spawn shock sources; count alive ────────
	int32 AliveNow = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		const FMassEntityHandle E = SpawnedEntities[i];
		const bool bValid = EM.IsEntityValid(E);

		bool bAlive = false;
		FVector Pos = FVector::ZeroVector;
		if (bValid)
		{
			const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(E);
			bAlive = (SF.State != EAgentState::DEAD);
			if (bAlive)
			{
				Pos = EM.GetFragmentDataChecked<FTransformFragment>(E).GetTransform().GetLocation();
				++AliveNow;
			}
			else
			{
				Pos = EM.GetFragmentDataChecked<FTransformFragment>(E).GetTransform().GetLocation();
			}
		}

		// Transition alive→dead this frame = a fresh casualty → emit shock,
		// scaled by how depleted the squad already was.
		if (SoldierWasAlive[i] && !bAlive)
		{
			ShockSources.Add({ Pos, ShockPerDeath * ShockLossScale });
			ExtendedCasualtyAccum += 1.f;
		}
		SoldierWasAlive[i] = bAlive;
	}

	// ── Pass 2: decay shock sources; drop the spent ones ────────────────────
	for (int32 s = ShockSources.Num() - 1; s >= 0; --s)
	{
		ShockSources[s].Strength -= ShockSources[s].Strength * ShockDecayRate * DeltaSeconds;
		if (ShockSources[s].Strength < 1.f)
			ShockSources.RemoveAtSwap(s);
	}

	// Extended-casualty accumulator: decays slowly (independent of the fast,
	// local ShockSources) so a STEADY trickle of deaths keeps outpacing the
	// decay and builds a lingering squad-wide drain — a sudden salvo (handled
	// by ShockSources) and the permanent ceiling (AttritionCeilingPenalty) are
	// separate mechanisms; this is the third, "sustained bleeding" timeframe.
	ExtendedCasualtyAccum = FMath::Max(0.f, ExtendedCasualtyAccum - ExtendedCasualtyDecayRate * DeltaSeconds);

	// ── Attrition ceiling (squad-wide, quadratic in losses) ─────────────────
	const float LossRatio = 1.f - (static_cast<float>(AliveNow) /
		static_cast<float>(InitialSoldierCount));
	const float Ceiling = FMath::Clamp(
		100.f - AttritionCeilingPenalty * LossRatio * LossRatio, 0.f, 100.f);

	const bool bHaveShocks = ShockSources.Num() > 0;
	if (!bHaveShocks && Ceiling >= 100.f && ExtendedCasualtyAccum <= 0.f) return;   // nothing to apply this frame

	const float FalloffSq = ShockFalloffRadius * ShockFalloffRadius;

	// ── Pass 3: apply shock drain + ceiling to each living soldier ──────────
	for (int32 i = 0; i < Count; ++i)
	{
		if (!SoldierWasAlive[i]) continue;   // dead — skip
		const FMassEntityHandle E = SpawnedEntities[i];
		if (!EM.IsEntityValid(E)) continue;

		const FVector MyPos = EM.GetFragmentDataChecked<FTransformFragment>(E)
			.GetTransform().GetLocation();
		FMoraleFragment& MF = EM.GetFragmentDataChecked<FMoraleFragment>(E);

		// Local casualty-shock drain — sum of nearby death emitters, each
		// weighted by linear distance falloff (1 at the corpse, 0 at radius).
		float Drain = 0.f;
		for (const FShockSource& Src : ShockSources)
		{
			const float DistSq = (Src.Pos - MyPos).SizeSquared2D();
			if (DistSq >= FalloffSq) continue;
			const float Falloff = 1.f - FMath::Sqrt(DistSq) / ShockFalloffRadius;
			Drain += Src.Strength * Falloff;
		}
		if (Drain > 0.f)
			MF.Morale = FMath::Max(0.f, MF.Morale - Drain * DeltaSeconds);

		// Extended-casualty drain — sustained losses over time, squad-wide,
		// independent of position (unlike the local ShockSources above).
		if (ExtendedCasualtyAccum > 0.f)
			MF.Morale = FMath::Max(0.f, MF.Morale - ExtendedCasualtyDrainRate * ExtendedCasualtyAccum * DeltaSeconds);

		// Attrition ceiling — morale can't sit above what the losses allow.
		if (MF.Morale > Ceiling)
			MF.Morale = Ceiling;
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

float ABattleSpawnerActor::GetClosestSoldierDistSq(const FVector& WorldPos) const
{
	UWorld* World = GetWorld();
	if (!World) return FLT_MAX;

	const UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return FLT_MAX;

	const FMassEntityManager& EM = Subsystem->GetEntityManager();

	float Best = FLT_MAX;
	const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());
	for (int32 i = 0; i < SoldierCount; ++i)
	{
		const FMassEntityHandle Entity = SpawnedEntities[i];
		if (!EM.IsEntityValid(Entity)) continue;

		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State == EAgentState::DEAD) continue;

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
		const float DistSq = (TF.GetTransform().GetLocation() - WorldPos).SizeSquared2D();
		if (DistSq < Best) Best = DistSq;
	}
	return Best;
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

int32 ABattleSpawnerActor::PurgeDesertersOutside(const FVector& BattlefieldCentre, float HalfExtent)
{
	UWorld* World = GetWorld();
	if (!World) return 0;

	UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return 0;

	FMassEntityManager& EM = Subsystem->GetMutableEntityManager();
	int32 Deserted = 0;

	for (const FMassEntityHandle& Entity : SpawnedEntities)
	{
		if (!EM.IsEntityValid(Entity)) continue;
		FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
		if (SF.State != EAgentState::ROUTING) continue;   // only routers desert

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
		const FVector P = TF.GetTransform().GetLocation();
		// Square map boundary: deserts if past the half-extent on either axis.
		if (FMath::Abs(P.X - BattlefieldCentre.X) > HalfExtent ||
			FMath::Abs(P.Y - BattlefieldCentre.Y) > HalfExtent)
		{
			// Fled the field — gone for good. Mark DEAD so every system
			// (counts, visualization, victory check) treats him as removed.
			SF.State      = EAgentState::DEAD;
			SF.StateTimer = 0.f;
			++Deserted;
		}
	}
	return Deserted;
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

	// Count each state (array sized to EAgentState count: 13, incl. WAVERING, STEADY)
	int32 StateCounts[13] = {};
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
	for (int32 s = 0; s < 13; ++s)
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
	case EAgentState::SHAKEN:    return TEXT("Chwieje sie");
	case EAgentState::WAVERING:  return TEXT("Niespokojny");
	case EAgentState::STEADY:    return TEXT("Stabilny");
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

	// Sync drummers to formation pace — they march with the line.
	for (const FMassEntityHandle& DrummerHandle : DrummerEntities)
	{
		if (!EM.IsEntityValid(DrummerHandle)) continue;
		FDrummerFragment& DR = EM.GetFragmentDataChecked<FDrummerFragment>(DrummerHandle);
		DR.MoveSpeed = FormationPace;
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

	// ── Gather only the LIVING soldiers (ranks close up over casualties) ──────
	TArray<FMassEntityHandle> LiveSoldiers;
	LiveSoldiers.Reserve(SpawnedEntities.Num());
	for (const FMassEntityHandle& E : SpawnedEntities)
	{
		if (!EM.IsEntityValid(E)) continue;
		const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(E);
		if (SF.State == EAgentState::DEAD) continue;
		LiveSoldiers.Add(E);
	}
	const int32 LiveCount = LiveSoldiers.Num();

	const int32 NumRows   = FMath::Max(1, FMath::CeilToInt((float)LiveCount / LocalRowSize));
	const float HalfFront = LocalRowSize * ColSpacing() * 0.5f;
	const float HalfDepth = NumRows      * SpawnSpacing * 0.5f;

	// Front line perpendicular to march direction (toward enemy)
	const FVector FrontLineDir = FVector(ToEnemy.Y, -ToEnemy.X, 0.f);
	const FQuat   FormRot      = FRotationMatrix::MakeFromX(FrontLineDir).ToQuat();

	// ── Assign each LIVING soldier a slot (k = packed live index) ─────────────
	for (int32 k = 0; k < LiveCount; ++k)
	{
		const FMassEntityHandle Entity = LiveSoldiers[k];

		const int32 Col = k % LocalRowSize;
		const int32 Row = k / LocalRowSize;
		const int32 InRow = FMath::Min(LocalRowSize, LiveCount - Row * LocalRowSize);
		const float CenterShift = (LocalRowSize - InRow) * ColSpacing() * 0.5f;  // centre partial last row
		const FVector LocalOffset(
			Col * ColSpacing() - HalfFront + CenterShift,
			HalfDepth - Row * SpawnSpacing,
			0.f
		);
		const FVector SlotPos = EngagePos + FormRot.RotateVector(LocalOffset);

		FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
		OF.TargetPosition  = SlotPos;
		OF.bHasTarget      = true;
		OF.FaceTarget      = EnemyCenter;
		OF.bHasFaceTarget  = true;

		// Seed the advance wave EXPLICITLY (front rank first), instead of relying
		// on the officer being close enough to "shout" the order the moment the
		// order processor next runs. The officer starts marching toward the enemy
		// immediately, so on an attack-move he can leave voice range before the
		// processor seeds anyone — which left the whole company standing while
		// only the officer advanced. A row-staggered delay reproduces the
		// front-to-back ripple deterministically. OrderProcessor's "order
		// received → ADVANCING" branch then drives every soldier.
		FOrderPropagationFragment& PF = EM.GetFragmentDataChecked<FOrderPropagationFragment>(Entity);
		PF.bOrderReceived = true;
		PF.bOrderExecuted = false;
		PF.bOrderIgnored  = false;
		PF.ExecutionTimer = 0.f;
		PF.ExecutionDelay = Row * 0.15f + FMath::FRandRange(0.f, 0.1f);

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
	SCOPE_CYCLE_COUNTER(STAT_BiedaEngage);
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
		const FFormationDims Dims = ComputeFormationDims(LocalRowSize);
		const int32 NumRows   = Dims.Rows;
		const float HalfFront = Dims.HalfFront;
		const float HalfDepth = Dims.HalfDepth;

		const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());
		for (int32 i = 0; i < SoldierCount; ++i)
		{
			const FMassEntityHandle Entity = SpawnedEntities[i];
			if (!EM.IsEntityValid(Entity)) continue;

			const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
			if (SF.State == EAgentState::DEAD) continue;

			const int32 Col = i % LocalRowSize;
			const int32 Row = i / LocalRowSize;
			const int32 InRow = FMath::Min(LocalRowSize, SoldierCount - Row * LocalRowSize);
			const float CenterShift = (LocalRowSize - InRow) * ColSpacing() * 0.5f;  // centre partial last row
			const FVector LocalOffset(
				Col * ColSpacing() - HalfFront + CenterShift,
				HalfDepth - Row * SpawnSpacing,
				0.f
			);

			FOrderFragment& OF = EM.GetFragmentDataChecked<FOrderFragment>(Entity);
			OF.TargetPosition = EngagePos + FormRot.RotateVector(LocalOffset);
			OF.bHasTarget     = true;

			// If out of range, force-advance soldiers who ALREADY heard the
			// engage order (bOrderExecuted=true) and are now idle (shooting/
			// holding). They already know they're fighting — this is just
			// chasing the enemy, not a new order.
			// DON'T touch soldiers still waiting for the initial propagation
			// wave (bOrderExecuted=false) — let the wave reach them naturally.
			if (bOutOfRange)
			{
				const FOrderPropagationFragment& PF =
					EM.GetFragmentDataChecked<FOrderPropagationFragment>(Entity);
				if (PF.bOrderExecuted)
				{
					FAgentStateFragment& SFMut =
						EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);
					if (SFMut.State == EAgentState::HOLDING ||
						SFMut.State == EAgentState::LOADING)
					{
						SFMut.State      = EAgentState::ADVANCING;
						SFMut.StateTimer = 0.f;
					}
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

		// Musicians: beside the officer at the front, follow him (was a gap —
		// engagement repositioned officer+NCO but left drummers behind).
		for (int32 d = 0; d < DrummerEntities.Num(); ++d)
		{
			if (!EM.IsEntityValid(DrummerEntities[d])) continue;
			FDrummerFragment& DR = EM.GetFragmentDataChecked<FDrummerFragment>(DrummerEntities[d]);
			const float SideStep = 150.f;
			const FVector MusLocal((d + 1) * SideStep, HalfDepth + 100.f, 0.f);
			DR.FormationPos     = EngagePos + FormRot.RotateVector(MusLocal);
			DR.bHasFormationPos = true;
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

void ABattleSpawnerActor::UpdateVolley(float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_BiedaVolley);
	if (VolleyMode == EVolleyMode::FreeFire) return;

	// RankFire: enforce a minimum gap between successive rank volleys. Without
	// it, on the FIRST engagement every rank finishes its synchronized reload on
	// the same frame, so the coordinator fires rank 0,1,2 on consecutive frames
	// — looking like one big volley. The gap spaces them into a visible rolling
	// fire. (Later volleys self-stagger via reload variance, but the gap keeps
	// them clean too.)
	constexpr float RankVolleyGap = 1.0f;   // seconds between rank volleys
	if (VolleyRankTimer > 0.f)
		VolleyRankTimer = FMath::Max(0.f, VolleyRankTimer - DeltaSeconds);

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
		// Hold fire until the gap since the last rank volley has elapsed.
		if (VolleyRankTimer <= 0.f)
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
					VolleyRankTimer   = RankVolleyGap;   // start gap before next rank
					break;
				}
				break;  // only check the current rank (wait for it)
			}
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

UInstancedStaticMeshComponent* ABattleSpawnerActor::CreateHISM(
	FName Name, UStaticMesh* Mesh, UMaterialInterface* Material)
{
	auto* HISM = NewObject<UInstancedStaticMeshComponent>(this, Name);
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
	UStaticMesh* DrumMesh  = DrummerMesh   ? DrummerMesh.Get()   : SolMesh;

	// When profiling: replace all detail meshes with a single fallback cylinder
	if (bUseDebugCapsules || BattleDebugCapsulesEnabled())
	{
		SolMesh  = Fallback;
		OffMesh  = Fallback;
		NcoMesh  = Fallback;
		DrumMesh = Fallback;
	}

	UMaterialInterface* NcoMat  = NCOMaterial      ? NCOMaterial.Get()      : SoldierMaterial.Get();
	UMaterialInterface* DrumMat = DrummerMaterial  ? DrummerMaterial.Get()  : SoldierMaterial.Get();
	UMaterialInterface* CorpMat = CorporalMaterial ? CorporalMaterial.Get() : SoldierMaterial.Get();

	SoldierHISM     = CreateHISM(TEXT("SoldierHISM"),     SolMesh, SoldierMaterial);
	DeadSoldierHISM = CreateHISM(TEXT("DeadSoldierHISM"), SolMesh, DeadSoldierMaterial);
	OfficerHISM     = CreateHISM(TEXT("OfficerHISM"),     OffMesh, SoldierMaterial);
	NCOHISM         = CreateHISM(TEXT("NCOHISM"),         NcoMesh, NcoMat);
	DrummerHISM     = CreateHISM(TEXT("DrummerHISM"),     DrumMesh, DrumMat);
	CorporalHISM    = CreateHISM(TEXT("CorporalHISM"),    SolMesh, CorpMat);

	// Debug capsules already drawn by BattleDebugProcessor when bieda.Debug=1;
	// visual meshes stay as cylinders so you see both.
}

void ABattleSpawnerActor::UpdateVisualization()
{
	SCOPE_CYCLE_COUNTER(STAT_BiedaVis);
	UWorld* World = GetWorld();
	if (!World) return;

	const UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!Subsystem) return;

	const FMassEntityManager& EM = Subsystem->GetEntityManager();

	// ── Soldiers (corporals routed to CorporalHISM — visual marker only) ────
	// PERF: gather transforms into arrays, then push each HISM in ONE batched
	// AddInstances call. A per-instance ClearInstances+AddInstance rebuilds
	// the HISM cluster tree on EVERY call (2000×/frame) — a game-thread killer.
	// AddInstances builds the tree once per HISM instead.
	//
	// Scale: SoldierMesh (Soldier_20K) is a real human-proportioned model
	// imported at 1:1 cm scale with feet at the local origin — unlike the old
	// engine-cylinder placeholder, it needs NO artificial XY squash / Z lift.
	// Hybrid switch: selected/near squads render via animated Characters
	// instead of the ISM. See AnimatedActorRange / ShouldUseAnimatedActors.
	if (ShouldUseAnimatedActors())
	{
		EnsureAnimatedActorsSpawned();
		SyncAnimatedActors(EM);

		if (SoldierHISM)      SoldierHISM->ClearInstances();
		if (DeadSoldierHISM)  DeadSoldierHISM->ClearInstances();
		if (CorporalHISM)     CorporalHISM->ClearInstances();
	}
	else if (SoldierHISM && DeadSoldierHISM)
	{
		if (AnimatedActors.Num() > 0) DestroyAnimatedActors();

		const int32 SoldierCount = FMath::Min(NumAgents, SpawnedEntities.Num());

		TArray<FTransform> AliveT, DeadT, CorpT;
		AliveT.Reserve(SoldierCount);
		DeadT.Reserve(SoldierCount / 4);
		if (CorporalHISM) CorpT.Reserve(NumCorporals);

		for (int32 i = 0; i < SoldierCount; ++i)
		{
			const FMassEntityHandle Entity = SpawnedEntities[i];
			if (!EM.IsEntityValid(Entity)) continue;

			const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(Entity);
			const FAgentStateFragment& SF = EM.GetFragmentDataChecked<FAgentStateFragment>(Entity);

			if (SF.State == EAgentState::DEAD)
			{
				// Fallen: rotate 90° pitch to lie down, flat on the ground.
				FVector Pos = TF.GetTransform().GetLocation();
				const float Yaw = TF.GetTransform().Rotator().Yaw;
				DeadT.Emplace(FRotator(0.f, Yaw, 90.f).Quaternion(), Pos, FVector(1.f, 1.f, 1.f));
			}
			else
			{
				const FTransform T(TF.GetTransform().GetRotation(), TF.GetTransform().GetLocation(),
					FVector(1.f, 1.f, 1.f));

				const bool bCorporal = CorporalHISM
					&& CorporalFlags.IsValidIndex(i) && CorporalFlags[i];
				if (bCorporal) CorpT.Add(T);
				else           AliveT.Add(T);
			}
		}

		SoldierHISM->ClearInstances();
		DeadSoldierHISM->ClearInstances();
		if (CorporalHISM) CorporalHISM->ClearInstances();

		// NOTE: instances are already absolute world transforms and the HISM
		// component sits at the origin (SetAbsolute), so add them in LOCAL
		// space (bWorldSpace=false) — bWorldSpace=true silently failed to
		// place them here. Still one batched call per HISM.
		if (AliveT.Num() > 0) SoldierHISM->AddInstances(AliveT, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
		if (DeadT.Num()  > 0) DeadSoldierHISM->AddInstances(DeadT, false, false);
		if (CorporalHISM && CorpT.Num() > 0) CorporalHISM->AddInstances(CorpT, false, false);
	}

	// ── Officer (HISM) ────────────────────────────────────────────────────
	if (OfficerHISM && OfficerEntity.IsValid() && EM.IsEntityValid(OfficerEntity))
	{
		OfficerHISM->ClearInstances();

		const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(OfficerEntity);
		const FOfficerFragment& OF   = EM.GetFragmentDataChecked<FOfficerFragment>(OfficerEntity);

		if (OF.bIsAlive)
		{
			FTransform OT(TF.GetTransform().GetRotation(), TF.GetTransform().GetLocation(),
				FVector(1.f, 1.f, 1.f));
			OfficerHISM->AddInstance(OT, true);
		}
	}

	// ── NCOs (HISM) ───────────────────────────────────────────────────────
	if (NCOHISM)
	{
		TArray<FTransform> NCOTs;
		NCOTs.Reserve(NCOEntities.Num());
		for (const FMassEntityHandle& NCOHandle : NCOEntities)
		{
			if (!EM.IsEntityValid(NCOHandle)) continue;

			const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(NCOHandle);
			const FNCOFragment& NCO      = EM.GetFragmentDataChecked<FNCOFragment>(NCOHandle);
			if (!NCO.bIsAlive) continue;

			NCOTs.Emplace(TF.GetTransform().GetRotation(), TF.GetTransform().GetLocation(),
				FVector(1.f, 1.f, 1.f));
		}

		NCOHISM->ClearInstances();
		if (NCOTs.Num() > 0) NCOHISM->AddInstances(NCOTs, false, true);
	}

	// ── Drummers (HISM) ───────────────────────────────────────────────────
	if (DrummerHISM)
	{
		TArray<FTransform> DrumTs;
		DrumTs.Reserve(DrummerEntities.Num());
		for (const FMassEntityHandle& DrummerHandle : DrummerEntities)
		{
			if (!EM.IsEntityValid(DrummerHandle)) continue;

			const FTransformFragment& TF = EM.GetFragmentDataChecked<FTransformFragment>(DrummerHandle);
			const FDrummerFragment& DR   = EM.GetFragmentDataChecked<FDrummerFragment>(DrummerHandle);
			if (!DR.bIsAlive) continue;

			DrumTs.Emplace(TF.GetTransform().GetRotation(), TF.GetTransform().GetLocation(),
				FVector(1.f, 1.f, 1.f));
		}

		DrummerHISM->ClearInstances();
		if (DrumTs.Num() > 0) DrummerHISM->AddInstances(DrumTs, false, true);
	}

	// ── Fire range arc + facing arrow (debug only) ───────────────────────
	if (bShowFireRange && BiedaDebugDrawEnabled())
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

bool ABattleSpawnerActor::DebugCapsulesDesired() const
{
	return bUseDebugCapsules || BattleDebugCapsulesEnabled();
}
