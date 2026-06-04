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
#include "BattleManager.h"
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
		ActionBtn  = FLinearColor(0.6f, 0.32f, 0.1f, 1.f);   // momentary commands (Stać)

		ChildSlot
		[
			SNew(SOverlay)

			// ══ Layer 1: normal HUD (unit panel + cards) ════════════════════
			+ SOverlay::Slot()
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
					.Padding(16.f)
					.Visibility_Lambda([this]() { return GetSelectedSpawner() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(SVerticalBox)

						// ── Header: name · state · HP ──────────────────────
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
							[
								SAssignNew(UnitNameText, STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 18))
								.ColorAndOpacity(FSlateColor(FLinearColor::White))
								.Text_Lambda([this]() { return FText::FromString(GetUnitName()); })
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 12.f, 0.f)
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 15))
								.ColorAndOpacity_Lambda([this]() -> FSlateColor
								{
									ABattleSpawnerActor* S = GetSelectedSpawner();
									if (!S) return FLinearColor::White;
									const FString St = S->GetDominantStateString();
									if (St == TEXT("PANIKA!"))   return FLinearColor::Red;
									if (St == TEXT("Strzela!"))  return FLinearColor::Yellow;
									if (St == TEXT("Maszeruje")) return FLinearColor(0.4f, 0.85f, 0.4f);
									return FLinearColor(0.82f, 0.82f, 0.9f);
								})
								.Text_Lambda([this]() -> FText
								{
									ABattleSpawnerActor* S = GetSelectedSpawner();
									return S ? FText::FromString(S->GetDominantStateString()) : FText::GetEmpty();
								})
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SAssignNew(HPText, STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.9f, 0.9f)))
								.Text_Lambda([this]() { return FText::FromString(GetHPString()); })
							]
						]

						// ── Morale bar (wide, % centred on the bar) ────────
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
						[
							SNew(SBox).HeightOverride(20.f).WidthOverride(360.f)
							[
								SNew(SOverlay)
								// BG track
								+ SOverlay::Slot()
								[
									SNew(SBorder)
									.BorderBackgroundColor(FLinearColor(0.08f, 0.08f, 0.1f, 1.f))
								]
								// Coloured fill
								+ SOverlay::Slot().HAlign(HAlign_Left)
								[
									SNew(SBox)
									.WidthOverride_Lambda([this]() -> FOptionalSize
									{
										return FOptionalSize(GetMoralePct() * 360.f);
									})
									[
										SNew(SBorder)
										.BorderBackgroundColor_Lambda([this]() -> FSlateColor
										{
											const float M = GetMoralePct();
											if (M > 0.6f) return FLinearColor(0.15f, 0.6f, 0.2f, 1.f);
											if (M > 0.3f) return FLinearColor(0.7f, 0.6f, 0.1f, 1.f);
											return FLinearColor(0.65f, 0.15f, 0.12f, 1.f);
										})
									]
								]
								// Centred label
								+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
									.ColorAndOpacity(FSlateColor(FLinearColor::White))
									.Text_Lambda([this]() -> FText
									{
										return FText::FromString(FString::Printf(TEXT("Morale %.0f%%"), GetMoralePct() * 100.f));
									})
								]
							]
						]

						// ── Command buttons: RUCH | OGIEŃ ──────────────────
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SHorizontalBox)

							// Movement group: Stać · Marsz · Bieg
							+ SHorizontalBox::Slot().AutoWidth()
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 0.f, 0.f, 3.f)
								[
									SNew(STextBlock)
									.Text(FText::FromString(TEXT("RUCH")))
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
									.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.62f)))
								]
								+ SVerticalBox::Slot().AutoHeight()
								[
									SNew(SHorizontalBox)
									+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 5.f, 0.f)
									[ MakeCommandButton(TEXT("Stać"), 6, /*bAction*/ true) ]
									+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 5.f, 0.f)
									[ MakeCommandButton(TEXT("Marsz"), 1) ]
									+ SHorizontalBox::Slot().AutoWidth()
									[ MakeCommandButton(TEXT("Bieg"), 2) ]
								]
							]

							// Gap between groups
							+ SHorizontalBox::Slot().AutoWidth().Padding(22.f, 0.f, 0.f, 0.f)
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 0.f, 0.f, 3.f)
								[
									SNew(STextBlock)
									.Text(FText::FromString(TEXT("OGIEŃ")))
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
									.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.62f)))
								]
								+ SVerticalBox::Slot().AutoHeight()
								[
									SNew(SHorizontalBox)
									+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 5.f, 0.f)
									[ MakeCommandButton(TEXT("Swobodny"), 3) ]
									+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 5.f, 0.f)
									[ MakeCommandButton(TEXT("Salwa"), 4) ]
									+ SHorizontalBox::Slot().AutoWidth()
									[ MakeCommandButton(TEXT("Rzędami"), 5) ]
								]
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
			]   // end Layer 1 (normal HUD)

			// ══ Layer 2: end-of-battle banner (centred, only when decided) ══
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.85f))
				.Padding(FMargin(60.f, 30.f))
				.Visibility_Lambda([this]() {
					return GetOutcomeText().IsEmpty() ? EVisibility::Collapsed
					                                  : EVisibility::HitTestInvisible;
				})
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 48))
					.Justification(ETextJustify::Center)
					.ColorAndOpacity_Lambda([this]() -> FSlateColor { return GetOutcomeColor(); })
					.Text_Lambda([this]() -> FText { return FText::FromString(GetOutcomeText()); })
				]
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
	FLinearColor ActionBtn;

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

	// ── End-of-battle banner ──────────────────────────────────────────────
	EBattleOutcome GetOutcome() const
	{
		if (!World) return EBattleOutcome::Ongoing;
		for (TActorIterator<ABattleManager> It(World); It; ++It)
			return It->GetOutcome();
		return EBattleOutcome::Ongoing;
	}

	FString GetOutcomeText() const
	{
		switch (GetOutcome())
		{
		case EBattleOutcome::PlayerVictory: return TEXT("ZWYCIĘSTWO");
		case EBattleOutcome::PlayerDefeat:  return TEXT("PORAŻKA");
		case EBattleOutcome::Draw:          return TEXT("REMIS");
		default:                            return FString();   // empty = hide banner
		}
	}

	FSlateColor GetOutcomeColor() const
	{
		switch (GetOutcome())
		{
		case EBattleOutcome::PlayerVictory: return FSlateColor(FLinearColor(0.3f, 1.f, 0.3f));
		case EBattleOutcome::PlayerDefeat:  return FSlateColor(FLinearColor(1.f, 0.25f, 0.25f));
		default:                            return FSlateColor(FLinearColor(0.85f, 0.85f, 0.5f));
		}
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

	TSharedRef<SWidget> MakeCommandButton(const FString& Label, int32 CmdIndex, bool bAction = false)
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
				.Padding(FMargin(14.f, 7.f))
				.HAlign(HAlign_Center)
				.BorderBackgroundColor_Lambda([this, CmdIndex, bAction]() -> FSlateColor
				{
					if (bAction) return ActionBtn;   // momentary action — fixed accent
					return IsCommandActive(CmdIndex) ? ActiveBtn : InactiveBtn;
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.Font_Lambda([this, CmdIndex, bAction]() -> FSlateFontInfo
					{
						return (bAction || IsCommandActive(CmdIndex))
							? FCoreStyle::GetDefaultFontStyle("Bold", 12)
							: FCoreStyle::GetDefaultFontStyle("Regular", 12);
					})
					.ColorAndOpacity_Lambda([this, CmdIndex, bAction]() -> FSlateColor
					{
						return (bAction || IsCommandActive(CmdIndex))
							? FSlateColor(FLinearColor::White)
							: FSlateColor(FLinearColor(0.72f, 0.72f, 0.78f));
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
		case 6: S->IssueHaltOrder(); break;   // "Stać" — halt & dress in place
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
