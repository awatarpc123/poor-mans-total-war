#include "BattleManager.h"
#include "BattleSpawnerActor.h"
#include "BattleSimControl.h"   // BattleSimPaused()
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"

ABattleManager::ABattleManager()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval  = 0.f;   // we throttle ourselves via ThinkTimer
}

void ABattleManager::BeginPlay()
{
	Super::BeginPlay();
	// Game opens on the main menu: freeze the simulation behind the overlay.
	GamePhase = EGamePhase::MainMenu;
	SetBattleSimPaused(true);
	// First think slightly delayed so all spawners have finished BeginPlay/spawn.
	ThinkTimer = 0.5f;
}

void ABattleManager::StartDeploy()
{
	GamePhase = EGamePhase::Deploy;
	// FULL freeze: units only stand — no marching, no firing, no AI. Placement
	// during deploy is instant (teleport), issued straight by the camera, so the
	// sim doesn't need to run.
	SetBattleSimPaused(true);
}

void ABattleManager::StartBattle()
{
	GamePhase = EGamePhase::Battle;
	SetBattleSimPaused(false);
	ThinkTimer = 0.1f;           // start thinking (AI/victory) almost immediately
}

void ABattleManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Map boundary + deploy zones are drawn even under the deploy pause.
	DrawFieldBounds();

	// Frozen with the simulation: no AI, no desertion, no victory flips on pause.
	if (BattleSimPaused()) return;

	// AI, desertion and victory checks run only once the battle has started —
	// not in the main menu or while the player is still deploying.
	if (GamePhase != EGamePhase::Battle) return;

	// Once decided, stop thinking — the battle is over.
	if (Outcome != EBattleOutcome::Ongoing) return;

	ThinkTimer -= DeltaSeconds;
	if (ThinkTimer > 0.f) return;
	ThinkTimer = ThinkInterval;

	Think();
}

void ABattleManager::Think()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// ── Gather every spawner, split by side, and purge deserters ────────────
	TArray<ABattleSpawnerActor*> PlayerSquads;
	TArray<ABattleSpawnerActor*> EnemySquads;
	int32 PlayerLiving = 0;
	int32 EnemyLiving  = 0;

	for (TActorIterator<ABattleSpawnerActor> It(World); It; ++It)
	{
		ABattleSpawnerActor* Sq = *It;
		if (!Sq) continue;

		// Routers who fled past the map boundary desert (removed from the fight).
		Sq->PurgeDesertersOutside(BattlefieldCentre, MapSize * 0.5f);

		const int32 Living = Sq->CountLiving();
		const bool  bPlayer = (Sq->TeamId == PlayerTeamId);

		if (bPlayer) { PlayerLiving += Living; if (Living > 0) PlayerSquads.Add(Sq); }
		else         { EnemyLiving  += Living; if (Living > 0) EnemySquads.Add(Sq); }
	}

	// ── Victory check ────────────────────────────────────────────────────────
	if (PlayerLiving == 0 && EnemyLiving == 0)
	{
		Outcome = EBattleOutcome::Draw;
		UE_LOG(LogTemp, Log, TEXT("BattleManager: DRAW — both sides annihilated."));
		return;
	}
	if (EnemyLiving == 0)
	{
		Outcome = EBattleOutcome::PlayerVictory;
		UE_LOG(LogTemp, Log, TEXT("BattleManager: PLAYER VICTORY."));
		return;
	}
	if (PlayerLiving == 0)
	{
		Outcome = EBattleOutcome::PlayerDefeat;
		UE_LOG(LogTemp, Log, TEXT("BattleManager: PLAYER DEFEAT."));
		return;
	}

	// ── Enemy AI ────────────────────────────────────────────────────────────
	// Defender: hold position; the combat system fires on its own when the player
	// closes in. No advancing.
	if (!bEnemyIsAggressor) return;

	// Aggressor: each idle squad picks a target, spreading attacks across the
	// player's squads (fewest attackers first, then nearest) instead of all
	// piling onto one. Sprint when far, march when close enough to fire in order.
	TMap<ABattleSpawnerActor*, int32> Attackers;
	for (ABattleSpawnerActor* T : PlayerSquads) Attackers.Add(T, 0);

	for (ABattleSpawnerActor* Enemy : EnemySquads)
	{
		// Already fighting a living target? keep it, but count the assignment.
		if (ABattleSpawnerActor* Cur = Enemy->GetEngagedTarget())
		{
			if (Cur->HasAliveSoldiers())
			{
				if (int32* C = Attackers.Find(Cur)) ++(*C);
				continue;
			}
		}

		const FVector EnemyCentre = Enemy->GetFormationCenter();
		ABattleSpawnerActor* Best = nullptr;
		float BestScore = FLT_MAX;
		for (ABattleSpawnerActor* Target : PlayerSquads)
		{
			const float Dist = FMath::Sqrt((Target->GetFormationCenter() - EnemyCentre).SizeSquared2D());
			// Spread out first (each existing attacker is a heavy penalty), then nearest.
			const float Score = Attackers[Target] * 1.0e6f + Dist;
			if (Score < BestScore) { BestScore = Score; Best = Target; }
		}

		if (Best)
		{
			const float Dist = FMath::Sqrt((Best->GetFormationCenter() - EnemyCentre).SizeSquared2D());
			Enemy->SetForceRun(Dist > AIRunDistance);   // sprint when far, march when close
			Enemy->IssueEngageOrder(Best);
			++Attackers[Best];
		}
	}
}

FBox ABattleManager::GetDeployZone(uint8 ForTeamId) const
{
	const float Half      = MapSize * 0.5f;
	const float ZoneHalfW = (MapSize * (2.f / 3.f)) * 0.5f;   // along Y (width)
	const float ZoneDepth =  MapSize * (1.f / 5.f);           // along X (depth)
	const bool  bPlayer   = (ForTeamId == PlayerTeamId);
	// Player's zone hugs the -X edge, enemy's the +X edge.
	const float Cx = bPlayer ? (-Half + ZoneDepth * 0.5f) : (Half - ZoneDepth * 0.5f);
	const FVector Center = BattlefieldCentre + FVector(Cx, 0.f, 0.f);
	const FVector Ext(ZoneDepth * 0.5f, ZoneHalfW, 1000.f);
	return FBox(Center - Ext, Center + Ext);
}

void ABattleManager::DrawFieldBounds() const
{
	UWorld* World = GetWorld();
	if (!World || GamePhase == EGamePhase::MainMenu) return;

	// Map boundary (square) — also the desertion line.
	const float Half = MapSize * 0.5f;
	DrawDebugBox(World, BattlefieldCentre, FVector(Half, Half, 500.f),
		FQuat::Identity, FColor(200, 200, 80), false, -1.f, 0, 40.f);

	// Deploy zones only while positioning the army.
	if (GamePhase != EGamePhase::Deploy) return;

	const uint8 EnemyTeam = (PlayerTeamId == 0) ? 1 : 0;
	const FBox PZ = GetDeployZone(PlayerTeamId);
	const FBox EZ = GetDeployZone(EnemyTeam);
	DrawDebugBox(World, PZ.GetCenter(), PZ.GetExtent(), FQuat::Identity,
		FColor(40, 160, 255), false, -1.f, 0, 60.f);
	DrawDebugBox(World, EZ.GetCenter(), EZ.GetExtent(), FQuat::Identity,
		FColor(255, 80, 60), false, -1.f, 0, 60.f);
}
