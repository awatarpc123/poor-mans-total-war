// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bieda_total_war.h"
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Containers/Ticker.h"
#include "UnrealClient.h"
#include "HAL/PlatformMisc.h"

// -BiedaAutoTest: map-independent screenshot + auto-quit hook for the
// unattended render test harness (see BattleGameMode.cpp for the test-squad
// half). Registered at engine init so it works on ANY map. Takes screenshots
// to Saved/Screenshots at ~8s and ~12s, quits at ~16s. Zero effect without
// the flag.
class FBiedaGameModule : public FDefaultGameModuleImpl
{
	virtual void StartupModule() override
	{
		FCoreDelegates::OnPostEngineInit.AddLambda([]()
		{
			if (!FParse::Param(FCommandLine::Get(), TEXT("BiedaAutoTest")))
				return;
			UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] module hook armed (screenshots + quit)"));
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[Elapsed = 0.f, Stage = 0](float Dt) mutable -> bool
				{
					Elapsed += Dt;
					if (Stage == 0 && Elapsed > 8.f)
					{ ++Stage; UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] shot 1")); FScreenshotRequest::RequestScreenshot(false); }
					else if (Stage == 1 && Elapsed > 12.f)
					{ ++Stage; UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] shot 2")); FScreenshotRequest::RequestScreenshot(false); }
					else if (Stage == 2 && Elapsed > 16.f)
					{
						++Stage;
						UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] done — quitting"));
						FPlatformMisc::RequestExit(false);
						return false;
					}
					return true;
				}));
		});
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE( FBiedaGameModule, Bieda_total_war, "Bieda_total_war" );
