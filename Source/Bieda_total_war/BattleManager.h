#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
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

	/** Player's team id (everything else is treated as hostile to the player). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Field")
	uint8 PlayerTeamId = 0;

	/** Seconds between brain ticks (AI re-targets, victory check). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Battle|Field", meta = (ClampMin = "0.25"))
	float ThinkInterval = 2.0f;

	/** Current outcome — Ongoing until one side is wiped. Read by the HUD. */
	EBattleOutcome GetOutcome() const { return Outcome; }

protected:
	virtual void BeginPlay() override;

private:
	float          ThinkTimer = 0.f;
	EBattleOutcome Outcome    = EBattleOutcome::Ongoing;

	void Think();
};
