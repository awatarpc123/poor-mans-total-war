#include "BattleGameMode.h"
#include "BattleCameraPawn.h"
#include "BattleHUD.h"
#include "BattleManager.h"
#include "EngineUtils.h"

ABattleGameMode::ABattleGameMode()
{
	DefaultPawnClass = ABattleCameraPawn::StaticClass();
	HUDClass = ABattleHUD::StaticClass();
}

void ABattleGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Ensure exactly one BattleManager exists. If the level designer placed one
	// (to tune BattlefieldRadius/centre per map), use it; otherwise spawn a
	// default so the game loop (AI + victory) always runs.
	UWorld* World = GetWorld();
	if (!World) return;

	for (TActorIterator<ABattleManager> It(World); It; ++It)
		return;   // a manager is already present — done

	World->SpawnActor<ABattleManager>(ABattleManager::StaticClass(),
		FTransform::Identity);
}
