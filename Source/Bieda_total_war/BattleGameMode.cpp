#include "BattleGameMode.h"
#include "BattleCameraPawn.h"
#include "BattleHUD.h"

ABattleGameMode::ABattleGameMode()
{
	DefaultPawnClass = ABattleCameraPawn::StaticClass();
	HUDClass = ABattleHUD::StaticClass();
}
