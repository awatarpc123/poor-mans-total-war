#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "MassEntityHandle.h"
#include "MassEntityManager.h"
#include "BattleTypes.h"
#include "BattleSpawnerActor.generated.h"

UCLASS()
class BIEDA_TOTAL_WAR_API ABattleSpawnerActor : public AActor
{
	GENERATED_BODY()

public:
	ABattleSpawnerActor();

	/** Unit type: Militia (loose, free fire) or Line Infantry (tight formation, volleys). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Unit")
	EUnitType UnitType = EUnitType::Militia;

	/** Firing mode: FreeFire (individual), SquadVolley (all at once), RankFire (row by row). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Unit")
	EVolleyMode VolleyMode = EVolleyMode::FreeFire;

	/** March speed (cm/s) — orderly walk in formation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Unit", meta = (ClampMin = "50", ClampMax = "500"))
	float MarchSpeed = 200.f;

	/** Run speed (cm/s) — fast advance, player picks this from UI. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Unit", meta = (ClampMin = "100", ClampMax = "700"))
	float RunSpeed = 400.f;

	/** Catch-up speed (cm/s) — automatic sprint when soldier falls behind formation slot.
	 *  Not exposed in UI; triggers itself when soldier is far from their slot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Unit", meta = (ClampMin = "300", ClampMax = "1000"))
	float CatchUpSpeed = 600.f;

	/** Organic curve: how much the formation line bends (cm). 0 = ruler-straight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Unit", meta = (ClampMin = "0", ClampMax = "200"))
	float FormationCurveStrength = 40.f;

	/** Line Infantry only: when formation is auto (RowSize = 0), form a 2-rank-deep
	 *  line — the British "thin red line". Ignored when RowSize is set explicitly
	 *  or when drag-to-form overrides the row count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Unit")
	bool bTwoRankLine = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
	int32 NumAgents = 50;

	/** Soldiers per row. 0 = auto (square root of NumAgents). For Line Infantry try 10-25. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn", meta = (ClampMin = "0", ClampMax = "100"))
	int32 RowSize = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
	float SpawnSpacing = 120.f;

	/** Number of corporals among the rank-and-file. Purely visual — they fight as
	 *  privates, but are marked with CorporalMaterial so the company reads as
	 *  organized. British establishment: ~5 per company. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn", meta = (ClampMin = "0", ClampMax = "20"))
	int32 NumCorporals = 5;

	/** Initial facing of all soldiers. Set Yaw = 180 for enemy formations facing the player. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
	FRotator SpawnFacing = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Spawn")
	float InitialMorale = 80.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Target")
	FVector TargetPosition = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Officer")
	bool bSpawnOfficer = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Officer")
	float OfficerInitialMorale = 95.f;

	/** Number of NCOs (sierżantów) per company. They stand behind the line as file
	 *  closers, dress the ranks, chase stragglers and rally routers. British
	 *  establishment: 4–5 per company. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|NCO", meta = (ClampMin = "0", ClampMax = "6"))
	int32 NumNCOs = 4;

	/** Number of drummers (doboszów) per company. They march with the line and
	 *  relay the officer's orders by drumbeat (extend order-propagation range).
	 *  British establishment: 2 per company. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Drummer", meta = (ClampMin = "0", ClampMax = "4"))
	int32 NumDrummers = 2;

	/** Drumbeat "voice" radius (cm) — soldiers of the same squad within this range
	 *  of a living drummer hear move orders directly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Drummer", meta = (ClampMin = "100", ClampMax = "2000"))
	float DrumVoiceRadius = 600.f;

	// ── Combat stats (per-spawner override) ─────────────────────────────────
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Combat")
	float SoldierHP = 100.f;

	/** Base accuracy (0.0–1.0). Randomized ±25% per soldier at spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Combat")
	float SoldierAccuracy = 0.15f;

	/** Seconds to reload musket. Randomized ±20% per soldier at spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Combat")
	float SoldierReloadTime = 15.f;

	/** Musket range in cm. 5000 = ~50 m. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Combat")
	float SoldierFireRange = 5000.f;

	/** Half-angle of the vision/fire cone (degrees). 90 = semicircle (180° FOV). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Combat", meta = (ClampMin = "10", ClampMax = "180"))
	float VisionHalfAngleDeg = 90.f;

	// ── Morale collapse (casualty shock + attrition ceiling) ────────────────
	/** Morale hit "intensity" emitted at each death spot. Nearby soldiers lose
	 *  this (×distance falloff ×time decay) per second. A sudden salvo stacks
	 *  many sources → instant rout; trickle deaths decay before stacking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Morale", meta = (ClampMin = "0"))
	float ShockPerDeath = 50.f;

	/** Radius (cm) of a death's shock. Beyond this, no effect. Keep small
	 *  (~1-2 ranks) so panic ripples back rank-by-rank, not all at once. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Morale", meta = (ClampMin = "50"))
	float ShockFalloffRadius = 350.f;

	/** How fast a death's shock fades (per second, exponential). Higher =
	 *  shorter-lived shock = trickle deaths matter less, salvos matter more. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Morale", meta = (ClampMin = "0.1"))
	float ShockDecayRate = 1.5f;

	/** Morale ceiling penalty at total wipe. Ceiling = 100 − this×lossRatio².
	 *  ~180 → unit's morale ceiling drops under the rout threshold at ~70%
	 *  losses (≈30% strength remaining) → slow attrition panic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Morale", meta = (ClampMin = "0"))
	float AttritionCeilingPenalty = 180.f;

	/** 0 = player, 1 = enemy, etc. Assigned to every entity via FFactionFragment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Faction")
	uint8 TeamId = 0;

	// ── Debug visualization ─────────────────────────────────────────────────
	/** Show fire range arc and facing arrow for this squad. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Debug")
	bool bShowFireRange = true;

	// ── Visual representation (ISM) ─────────────────────────────────────────
	/** Mesh for soldiers. If nullptr, uses engine cylinder placeholder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Visuals")
	TObjectPtr<UStaticMesh> SoldierMesh = nullptr;

	/** Material for alive soldiers. If nullptr, uses mesh default material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Visuals")
	TObjectPtr<UMaterialInterface> SoldierMaterial = nullptr;

	/** Material for dead soldiers. If nullptr, uses a dark gray fallback. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Visuals")
	TObjectPtr<UMaterialInterface> DeadSoldierMaterial = nullptr;

	/** Mesh for officer. If nullptr, uses SoldierMesh or cylinder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Visuals")
	TObjectPtr<UStaticMesh> OfficerMesh = nullptr;

	/** Mesh for NCOs. If nullptr, uses SoldierMesh or cylinder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Visuals")
	TObjectPtr<UStaticMesh> NCOMesh = nullptr;

	/** Material for NCOs (e.g. brighter color to stand out). If nullptr, uses SoldierMaterial. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Visuals")
	TObjectPtr<UMaterialInterface> NCOMaterial = nullptr;

	/** Mesh for drummers. If nullptr, uses SoldierMesh or cylinder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Visuals")
	TObjectPtr<UStaticMesh> DrummerMesh = nullptr;

	/** Material for drummers (e.g. distinct color). If nullptr, uses SoldierMaterial. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Visuals")
	TObjectPtr<UMaterialInterface> DrummerMaterial = nullptr;

	/** Material for corporals (subtle marker color). If nullptr, uses SoldierMaterial. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Visuals")
	TObjectPtr<UMaterialInterface> CorporalMaterial = nullptr;

	/**
	 * Issue a move order with optional formation change.
	 * Clears any active engagement.
	 * @param NewWorldTarget  Center of the formation at destination
	 * @param InRowSize       Soldiers per front-line row (-1 = keep current)
	 * @param InFrontLineDir  Direction of the front line (zero = auto from center→target)
	 */
	void IssueMoveOrder(const FVector& NewWorldTarget, int32 InRowSize = -1,
		const FVector& InFrontLineDir = FVector::ZeroVector);

	/**
	 * Engage an enemy squad — auto-approach, stop at fire range, chase if enemy moves.
	 * Cleared by a ground move order (IssueMoveOrder).
	 */
	void IssueEngageOrder(ABattleSpawnerActor* EnemySquad);

	/** Stop all soldiers in place — clears current movement & engagement.
	 *  Called when the player starts a new click/drag interaction. */
	void HoldPosition();

	/** Halt-and-dress ("Stać"): re-form the line exactly where it currently
	 *  stands. The front rank is anchored on the MOST-ADVANCED soldier (he is
	 *  rank 1), and everyone dresses forward to that line — no centroid step-back.
	 *  The order still travels through the officer→drummer→peer wave, so the unit
	 *  stops with a natural stagger and bleeds off running speed. Works for both
	 *  militia and line infantry. */
	void IssueHaltOrder();

	/** Average world position of all living soldiers (used for selection). */
	FVector GetFormationCenter() const;

	/** Are there any alive soldiers? (used by engagement system). */
	bool HasAliveSoldiers() const;

	/** Currently engaged enemy (nullptr = none). */
	ABattleSpawnerActor* GetEngagedTarget() const { return EngagedTarget; }

	/** Current soldiers-per-row in the formation (used for preview). */
	int32 GetCurrentRowSize() const { return CurrentRowSize; }

	// ── UI getters ──────────────────────────────────────────────────────────
	int32 GetAliveCount() const;
	float GetAverageMorale() const;
	FString GetDominantStateString() const;

	// ── Runtime commands (called from UI / keyboard) ────────────────────────
	void SetVolleyModeRuntime(EVolleyMode NewMode);
	void SetForceRun(bool bRun);

	/** Force-run flag: all soldiers use RunSpeed regardless of distance to slot. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Battle|Unit")
	bool bForceRun = false;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	TArray<FMassEntityHandle> SpawnedEntities;   // soldiers only
	FMassEntityHandle OfficerEntity;              // officer (invalid if not spawned)
	TArray<FMassEntityHandle> NCOEntities;        // NCOs (sierżanci)
	TArray<FMassEntityHandle> DrummerEntities;    // drummers (doboszowie)
	TArray<bool> CorporalFlags;                   // parallel to SpawnedEntities — visual corporal marker
	uint8 MySquadId = 0;
	int32 CurrentRowSize = -1;   // set at spawn, updated by drag-to-form
	void SpawnAgents();

	// ── Formation geometry (single source of truth) ──────────────────────────
	// The slot math (rows, half-width, half-depth from a given row size) used to
	// be copy-pasted across SpawnAgents / IssueMoveOrder / IssueEngageOrder /
	// UpdateEngagement, which risked the layouts silently drifting apart.
	// Compute it in one place instead.
	struct FFormationDims
	{
		int32 Cols;        // soldiers per front-line row
		int32 Rows;        // depth (number of ranks)
		float HalfFront;   // half formation width  (cm)
		float HalfDepth;   // half formation depth  (cm)
	};
	FFormationDims ComputeFormationDims(int32 InRowSize) const;

	// ── Casualty shock + morale collapse ─────────────────────────────────────
	// A transient morale-hit emitter dropped at the spot where a soldier died.
	// Soldiers near it bleed morale (distance falloff); the source fades over
	// time. Local + decaying → a salvo (many at once) routs the survivors near
	// the gap and the panic ripples back rank-by-rank (no squad-wide sync).
	struct FShockSource { FVector Pos; float Strength; };
	TArray<FShockSource> ShockSources;     // active death-shock emitters
	TArray<bool>         SoldierWasAlive;   // per-soldier alive state last frame
	int32                InitialSoldierCount = 0;  // for the attrition ceiling
	void UpdateCasualtyShock(float DeltaSeconds);

	// ── Engagement ──────────────────────────────────────────────────────────
	UPROPERTY()
	TObjectPtr<ABattleSpawnerActor> EngagedTarget;

	FVector LastEngageEnemyPos = FVector::ZeroVector;   // throttle re-orders
	void UpdateEngagement();
	void SetFaceTargetOnSoldiers(const FVector& WorldTarget);
	void ClearFaceTarget();

	// ── Volley coordination ─────────────────────────────────────────────────
	int32 CurrentVolleyRank = 0;   // for RankFire: which row fires next
	void UpdateVolley();

	// ── Straggler detection ─────────────────────────────────────────────────
	// Flags soldiers that have fallen far behind the formation centroid so
	// the movement processor can apply CatchUpSpeed for them.
	void UpdateStragglers();

	// ── Visualization ───────────────────────────────────────────────────────
	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> SoldierHISM;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> DeadSoldierHISM;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> OfficerHISM;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> NCOHISM;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> DrummerHISM;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> CorporalHISM;

	void SetupVisualization();
	void UpdateVisualization();

	UStaticMesh* GetFallbackMesh() const;
	UHierarchicalInstancedStaticMeshComponent* CreateHISM(
		FName Name, UStaticMesh* Mesh, UMaterialInterface* Material);

	/** Draw fire-range arc and facing arrow (called from UpdateVisualization). */
	void DrawFireRangeArc(const FMassEntityManager& EM);
};
