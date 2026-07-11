// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bieda_total_war.h"
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Containers/Ticker.h"
#include "UnrealClient.h"
#include "HAL/PlatformMisc.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "BattleSpawnerActor.h"

// -BiedaAutoTest: map-independent screenshot + auto-quit hook for the
// unattended render test harness (see BattleGameMode.cpp for the test-squad
// half). Registered at engine init so it works on ANY map. At ~4s it aims the
// player camera at the first squad (possession has settled by then — doing it
// in GameMode::BeginPlay was too early and silently failed). Screenshots at
// ~8s and ~12s, quits at ~16s. Zero effect without the flag.
class FBiedaGameModule : public FDefaultGameModuleImpl
{
	static UWorld* FindGameWorld()
	{
		if (!GEngine) return nullptr;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE)
				return Ctx.World();
		}
		return nullptr;
	}

	virtual void StartupModule() override
	{
		FCoreDelegates::OnPostEngineInit.AddLambda([]()
		{
			if (!FParse::Param(FCommandLine::Get(), TEXT("BiedaAutoTest")))
				return;
			UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] module hook armed (camera + screenshots + quit)"));
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[Elapsed = 0.f, Stage = 0](float Dt) mutable -> bool
				{
					Elapsed += Dt;
					if (Stage == 0 && Elapsed > 4.f)
					{
						++Stage;
						// Aim the (now-possessed) camera pawn at the first squad.
						if (UWorld* World = FindGameWorld())
						{
							ABattleSpawnerActor* Squad = nullptr;
							for (TActorIterator<ABattleSpawnerActor> It(World); It; ++It) { Squad = *It; break; }
							APlayerController* PC = World->GetFirstPlayerController();
							APawn* CamPawn = PC ? PC->GetPawn() : nullptr;
							if (Squad && CamPawn)
							{
								const FVector Target = Squad->GetFormationCenter() + FVector(0, 0, 120.f);
								const FVector CamLoc = Target + FVector(-900.f, -500.f, 220.f);
								const FRotator LookRot = (Target - CamLoc).Rotation();
								CamPawn->SetActorLocationAndRotation(CamLoc, LookRot);
								PC->SetControlRotation(LookRot);
								UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] camera aimed at %s"), *Target.ToCompactString());
							}
							else
							{
								UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] camera aim skipped (squad=%d pawn=%d)"),
									Squad != nullptr, CamPawn != nullptr);
							}
						}
					}
					else if (Stage == 1 && Elapsed > 8.f)
					{ ++Stage; UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] shot 1")); FScreenshotRequest::RequestScreenshot(false); }
					else if (Stage == 2 && Elapsed > 12.f)
					{ ++Stage; UE_LOG(LogTemp, Warning, TEXT("[BiedaAutoTest] shot 2")); FScreenshotRequest::RequestScreenshot(false); }
					else if (Stage == 3 && Elapsed > 16.f)
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
