#include "BattleHUD.h"
#include "SBattleHUDWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"

void ABattleHUD::BeginPlay()
{
	Super::BeginPlay();

	if (GEngine && GEngine->GameViewport)
	{
		HUDWidget = SNew(SBattleHUDWidget).OwnerWorld(GetWorld());

		GEngine->GameViewport->AddViewportWidgetContent(
			HUDWidget.ToSharedRef(), 100);
	}
}
