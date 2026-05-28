#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Styling/SlateColor.h"
#include "EngineUtils.h"
#include "BattleSpawnerActor.h"
#include "BattleCameraPawn.h"
#include "GameFramework/PlayerController.h"

/**
 * Battle HUD — Total War style.
 * Bottom panel with selected unit info + command buttons.
 * Bottom-right: unit cards for all friendly squads.
 */
class SBattleHUDWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBattleHUDWidget)
		: _OwnerWorld(nullptr)
	{}
		SLATE_ARGUMENT(UWorld*, OwnerWorld)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		World = InArgs._OwnerWorld;

		// ── Styles ───────────────────────────────────────────────────────
		PanelBG = FLinearColor(0.02f, 0.02f, 0.05f, 0.95f);
		CardBG  = FLinearColor(0.05f, 0.05f, 0.08f, 0.93f);
		ActiveBtn  = FLinearColor(0.08f, 0.35f, 0.65f, 1.f);
		InactiveBtn = FLinearColor(0.22f, 0.22f, 0.28f, 1.f);

		ChildSlot
		[
			SNew(SVerticalBox)

			// Spacer pushes everything to bottom
			+ SVerticalBox::Slot().FillHeight(1.f)

			// ── Bottom area ──────────────────────────────────────────────
			+ SVerticalBox::Slot().AutoHeight().Padding(20.f, 0.f, 20.f, 20.f)
			[
				SNew(SHorizontalBox)

				// Left spacer
				+ SHorizontalBox::Slot().FillWidth(0.15f)

				// ── Unit panel (center) ──────────────────────────────────
				+ SHorizontalBox::Slot().FillWidth(0.5f)
				[
					SAssignNew(UnitPanelBorder, SBorder)
					.BorderBackgroundColor(PanelBG)
					.Padding(12.f)
					.Visibility_Lambda([this]() { return GetSelectedSpawner() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(SVerticalBox)

						// Row 1: Unit name + HP
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.f)
							[
								SAssignNew(UnitNameText, STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
								.ColorAndOpacity(FSlateColor(FLinearColor::White))
								.Text_Lambda([this]() { return FText::FromString(GetUnitName()); })
							]
							+ SHorizontalBox::Slot().AutoWidth()
							[
								SAssignNew(HPText, STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
								.ColorAndOpacity(FSlateColor(FLinearColor::White))
								.Text_Lambda([this]() { return FText::FromString(GetHPString()); })
							]
						]

						// Row 2: Morale bar + State
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(0.6f).VAlign(VAlign_Center)
							[
								SNew(SBox).HeightOverride(16.f)
								[
									SNew(SOverlay)
									// BG
									+ SOverlay::Slot()
									[
										SNew(SBorder)
										.BorderBackgroundColor(FLinearColor(0.1f, 0.1f, 0.1f, 1.f))
									]
									// Fill
									+ SOverlay::Slot()
									[
										SNew(SBox)
										.HAlign(HAlign_Left)
										.WidthOverride_Lambda([this]() -> FOptionalSize
										{
											const float Pct = GetMoralePct();
											return FOptionalSize(Pct * 200.f);
										})
										[
											SNew(SBorder)
											.BorderBackgroundColor_Lambda([this]() -> FSlateColor
											{
												const float M = GetMoralePct();
												if (M > 0.6f) return FLinearColor::Green;
												if (M > 0.3f) return FLinearColor::Yellow;
												return FLinearColor::Red;
											})
										]
									]
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 0.f, 0.f)
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.8f, 0.8f, 0.8f)))
								.Text_Lambda([this]() -> FText
								{
									return FText::FromString(FString::Printf(TEXT("Morale: %.0f%%"), GetMoralePct() * 100.f));
								})
							]
							+ SHorizontalBox::Slot().FillWidth(0.4f).HAlign(HAlign_Right)
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
								.ColorAndOpacity_Lambda([this]() -> FSlateColor
								{
									ABattleSpawnerActor* S = GetSelectedSpawner();
									if (!S) return FLinearColor::White;
									const FString St = S->GetDominantStateString();
									if (St == TEXT("PANIKA!"))    return FLinearColor::Red;
									if (St == TEXT("Strzela!"))   return FLinearColor::Yellow;
									if (St == TEXT("Maszeruje"))  return FLinearColor::Green;
									return FLinearColor::White;
								})
								.Text_Lambda([this]() -> FText
								{
									ABattleSpawnerActor* S = GetSelectedSpawner();
									return S ? FText::FromString(S->GetDominantStateString()) : FText::GetEmpty();
								})
							]
						]

						// Row 3: Command buttons
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SHorizontalBox)

							// Movement group
							+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
							[
								MakeCommandButton(TEXT("Marsz"), 1)
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 16.f, 0.f)
							[
								MakeCommandButton(TEXT("Bieg"), 2)
							]

							// Separator
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 16.f, 0.f)
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("|")))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
							]

							// Fire mode group
							+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
							[
								MakeCommandButton(TEXT("Swobodny"), 3)
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
							[
								MakeCommandButton(TEXT("Salwa"), 4)
							]
							+ SHorizontalBox::Slot().AutoWidth()
							[
								MakeCommandButton(TEXT("Rzedami"), 5)
							]
						]
					]
				]

				// Spacer between panel and cards
				+ SHorizontalBox::Slot().FillWidth(0.05f)

				// ── Unit cards (right side) ──────────────────────────────
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
				[
					SAssignNew(CardsBox, SHorizontalBox)
				]

				// Right spacer
				+ SHorizontalBox::Slot().FillWidth(0.05f)
			]
		];
	}

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
	{
		SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
		RebuildCards();
	}

private:
	UWorld* World = nullptr;

	// Styles
	FLinearColor PanelBG;
	FLinearColor CardBG;
	FLinearColor ActiveBtn;
	FLinearColor InactiveBtn;

	// Widget refs
	TSharedPtr<SBorder> UnitPanelBorder;
	TSharedPtr<STextBlock> UnitNameText;
	TSharedPtr<STextBlock> HPText;
	TSharedPtr<SHorizontalBox> CardsBox;

	// ── Helpers ──────────────────────────────────────────────────────────

	ABattleCameraPawn* GetCameraPawn() const
	{
		if (!World) return nullptr;
		APlayerController* PC = World->GetFirstPlayerController();
		if (!PC) return nullptr;
		return Cast<ABattleCameraPawn>(PC->GetPawn());
	}

	ABattleSpawnerActor* GetSelectedSpawner() const
	{
		ABattleCameraPawn* Cam = GetCameraPawn();
		return Cam ? Cam->GetSelectedSpawner() : nullptr;
	}

	FString GetUnitName() const
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return TEXT("");
		return (S->UnitType == EUnitType::LineInfantry)
			? TEXT("Piechota Liniowa")
			: TEXT("Milicja");
	}

	FString GetHPString() const
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return TEXT("");
		return FString::Printf(TEXT("HP: %d / %d"), S->GetAliveCount(), S->NumAgents);
	}

	float GetMoralePct() const
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return 0.f;
		return FMath::Clamp(S->GetAverageMorale() / 100.f, 0.f, 1.f);
	}

	// ── Button helper ────────────────────────────────────────────────────

	TSharedRef<SWidget> MakeCommandButton(const FString& Label, int32 CmdIndex)
	{
		return SNew(SButton)
			.ContentPadding(0.f)
			.ButtonColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.f))
			.OnClicked_Lambda([this, CmdIndex]() -> FReply
			{
				ExecuteCommand(CmdIndex);
				return FReply::Handled();
			})
			[
				SNew(SBorder)
				.Padding(FMargin(12.f, 6.f))
				.BorderBackgroundColor_Lambda([this, CmdIndex]() -> FSlateColor
				{
					return IsCommandActive(CmdIndex) ? ActiveBtn : InactiveBtn;
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.Font_Lambda([this, CmdIndex]() -> FSlateFontInfo
					{
						return IsCommandActive(CmdIndex)
							? FCoreStyle::GetDefaultFontStyle("Bold", 12)
							: FCoreStyle::GetDefaultFontStyle("Regular", 11);
					})
					.ColorAndOpacity_Lambda([this, CmdIndex]() -> FSlateColor
					{
						return IsCommandActive(CmdIndex)
							? FSlateColor(FLinearColor::White)
							: FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f));
					})
				]
			];
	}

	void ExecuteCommand(int32 CmdIndex)
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return;

		switch (CmdIndex)
		{
		case 1: S->SetForceRun(false); break;
		case 2: S->SetForceRun(true); break;
		case 3: S->SetVolleyModeRuntime(EVolleyMode::FreeFire); break;
		case 4: S->SetVolleyModeRuntime(EVolleyMode::SquadVolley); break;
		case 5: S->SetVolleyModeRuntime(EVolleyMode::RankFire); break;
		}
	}

	bool IsCommandActive(int32 Cmd) const
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return false;

		switch (Cmd)
		{
		case 1: return !S->bForceRun;   // Marsz active when NOT running
		case 2: return S->bForceRun;    // Bieg active when running
		case 3: return S->VolleyMode == EVolleyMode::FreeFire;
		case 4: return S->VolleyMode == EVolleyMode::SquadVolley;
		case 5: return S->VolleyMode == EVolleyMode::RankFire;
		default: return false;
		}
	}

	// ── Unit cards ───────────────────────────────────────────────────────

	void RebuildCards()
	{
		if (!CardsBox.IsValid() || !World) return;

		// Collect friendly spawners
		TArray<ABattleSpawnerActor*> Friendly;
		for (TActorIterator<ABattleSpawnerActor> It(World); It; ++It)
		{
			if (It->TeamId == 0)
				Friendly.Add(*It);
		}

		// Only rebuild if count changed (performance)
		if (Friendly.Num() == LastCardCount) return;
		LastCardCount = Friendly.Num();

		CardsBox->ClearChildren();

		for (int32 i = 0; i < Friendly.Num(); ++i)
		{
			ABattleSpawnerActor* Spawner = Friendly[i];

			CardsBox->AddSlot()
			.AutoWidth()
			.Padding(2.f)
			[
				SNew(SButton)
				.ContentPadding(0.f)
				.ButtonColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.f))
				.OnClicked_Lambda([this, Spawner]() -> FReply
				{
					ABattleCameraPawn* Cam = GetCameraPawn();
					if (Cam) Cam->SetSelectedSpawner(Spawner);
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.WidthOverride(70.f)
					.HeightOverride(80.f)
					[
						SNew(SBorder)
						.BorderBackgroundColor_Lambda([this, Spawner]() -> FSlateColor
						{
							return (GetSelectedSpawner() == Spawner)
								? FLinearColor(0.1f, 0.6f, 0.9f, 0.9f)
								: CardBG;
						})
						.Padding(4.f)
						[
							SNew(SVerticalBox)

							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
								.ColorAndOpacity(FSlateColor(FLinearColor::White))
								.Text(FText::FromString(FString::Printf(TEXT("%d"), i + 1)))
							]

							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.8f, 0.8f, 0.8f)))
								.Text_Lambda([Spawner]() -> FText
								{
									return FText::FromString(FString::Printf(TEXT("%d"), Spawner->GetAliveCount()));
								})
							]

							+ SVerticalBox::Slot().FillHeight(1.f)

							// Mini morale bar
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SBox).HeightOverride(6.f)
								[
									SNew(SOverlay)
									+ SOverlay::Slot()
									[
										SNew(SBorder).BorderBackgroundColor(FLinearColor(0.1f, 0.1f, 0.1f))
									]
									+ SOverlay::Slot()
									[
										SNew(SBox).HAlign(HAlign_Left)
										.WidthOverride_Lambda([Spawner]() -> FOptionalSize
										{
											const float M = FMath::Clamp(Spawner->GetAverageMorale() / 100.f, 0.f, 1.f);
											return FOptionalSize(M * 62.f);
										})
										[
											SNew(SBorder)
											.BorderBackgroundColor_Lambda([Spawner]() -> FSlateColor
											{
												const float M = Spawner->GetAverageMorale();
												if (M > 60.f) return FLinearColor::Green;
												if (M > 30.f) return FLinearColor::Yellow;
												return FLinearColor::Red;
											})
										]
									]
								]
							]
						]
					]
				]
			];
		}
	}

	int32 LastCardCount = -1;
};
