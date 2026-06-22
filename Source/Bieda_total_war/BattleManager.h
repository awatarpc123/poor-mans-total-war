#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BattleTypes.h"          // EUnitType
#include "BattleManager.generated.h"

class ABattleSpawnerActor;

UENUM()
enum class EBattleOutcome : uint8
{
	Ongoing,
	PlayerVictory,
	PlayerDefeat,
	Draw          // both sides wiped (mutual annihilation)
};

UENUM()
enum class EGamePhase : uint8
{
	MainMenu,   // start screen — simulation frozen behind the menu overlay
	ArmySetup,  // pick army composition (player + enemy) before deploying
	Deploy,     // arrange your units; sim frozen, placement is instant
	Battle      // full battle: enemy AI + victory checks active
};

/**
 * The battle "brain": one coordinator actor (spawned by the GameMode) that, on
 * a slow tick (~2s), drives the parts of the game loop that need a global view:
 *
 *   1. ENEMY AI — every enemy squad (TeamId != 0) that isn't already engaged
 *      finds the nearest living player squad and issues an attack (the same
 *      IssueEngageOrder the player's click uses).
 *   2. DESERTION — routing soldiers who flee past the battlefield radius are
 *      removed (PurgeDesertersOutside).
 *   3. VICTORY — counts living soldiers per side; when one side hits zero the
 *      battle ends and the outcome is recorded (read by the HUD for the banner).
 *
 * Respects the tactical pause (skips its tick while BattleSimPaused()).
 */
UCLASS()
class BIEDA_TOTAL_WAR_API ABattleManager : public AActor
{
	GENERATED_BODY()

public:
	ABattleManager();

	virtual void Tick(float DeltaSeconds) override;

	/** Battlefield centre + radius (cm). Routers beyond this desert; the radius
	 *  also bounds "is this squad still on the field". Editable per-level. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Field")
	FVector BattlefieldCentre = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Field", meta = (ClampMin = "1000"))
	float BattlefieldRadius = 20000.f;

	/** Full map size (cm), square. Default = musket range (5000) * 2 * 10 = 100000.
	 *  This is ALSO the battlefield boundary: routers who cross it desert. Deploy
	 *  zones are derived from it (2/3 of the width, 1/5 of the length per side). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Field", meta = (ClampMin = "1000"))
	float MapSize = 100000.f;

	/** World-space deploy zone (Z ignored) for a side. Player's is at the -X edge,
	 *  enemy's at +X; width = 2/3 of MapSize (along Y), depth = 1/5 (along X). */
	FBox GetDeployZone(uint8 ForTeamId) const;

	/** Player's team id (everything else is treated as hostile to the player). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Field")
	uint8 PlayerTeamId = 0;

	/** Seconds between brain ticks (AI re-targets, victory check). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Field", meta = (ClampMin = "0.25"))
	float ThinkInterval = 2.0f;

	/** Who attacks: true = the AI side advances on the player (player defends);
	 *  false = AI holds position and only fires when the player closes in.
	 *  Will be set by the army-setup screen later. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|AI")
	bool bEnemyIsAggressor = true;

	/** AI sprints toward a target farther than this (cm), then drops to a march
	 *  to close and fire in order. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|AI", meta = (ClampMin = "0"))
	float AIRunDistance = 8000.f;

	/** Soldiers per Militia squad spawned from the army-setup screen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|AI", meta = (ClampMin = "1"))
	int32 MilitiaSoldiersPerSquad = 150;

	/** Soldiers per Line Infantry squad spawned from the army-setup screen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|AI", meta = (ClampMin = "1"))
	int32 LineSoldiersPerSquad = 120;

	/** Current outcome — Ongoing until one side is wiped. Read by the HUD. */
	EBattleOutcome GetOutcome() const { return Outcome; }

	/** Current game phase (menu / deploy / battle). Read by the HUD. */
	EGamePhase GetGamePhase() const { return GamePhase; }

	/** Main-menu "GRAJ" → enter deployment: sim resumes so you can position
	 *  your units, but enemy AI and victory checks stay off. */
	void StartDeploy();

	/** Deployment "ROZPOCZNIJ BITWĘ" → begin the battle proper (AI on). */
	void StartBattle();

	// ── Army setup (custom battle) ──────────────────────────────────────────
	/** Main-menu "GRAJ" → army selection screen (sim stays frozen). */
	void StartArmySetup();

	/** Add / remove one squad of a type to a side (player or enemy). */
	void AddSquad(bool bPlayer, EUnitType Type);
	void RemoveSquad(bool bPlayer, EUnitType Type);

	/** Squad count of a type on a side — read by the setup UI. */
	int32 GetSquadCount(bool bPlayer, EUnitType Type) const;

	/** Confirm composition → spawn both armies in their zones → enter Deploy. */
	void ConfirmArmiesAndDeploy();

protected:
	virtual void BeginPlay() override;

private:
	float          ThinkTimer = 0.f;
	EBattleOutcome Outcome    = EBattleOutcome::Ongoing;
	EGamePhase     GamePhase  = EGamePhase::MainMenu;

	// Army composition chosen on the setup screen (squad counts per side/type).
	int32 PlayerMilitiaSquads = 0;
	int32 PlayerLineSquads    = 0;
	int32 EnemyMilitiaSquads  = 0;
	int32 EnemyLineSquads     = 0;

	void Think();
	void SpawnArmies();
	void SpawnSideArmy(uint8 ForTeamId, float YawDeg, int32 NumMilitia, int32 NumLine);

	/** Draws the map boundary (always, outside the menu) and the two deploy
	 *  zones (only while deploying). Runs even under the deploy pause. */
	void DrawFieldBounds() const;
};
