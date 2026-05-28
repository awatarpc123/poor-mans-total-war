#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "BattleHUD.generated.h"

class SBattleHUDWidget;

UCLASS()
class BIEDA_TOTAL_WAR_API ABattleHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;

private:
	TSharedPtr<SBattleHUDWidget> HUDWidget;
};
