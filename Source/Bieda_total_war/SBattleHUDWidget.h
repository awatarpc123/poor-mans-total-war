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
#include "BattleSimControl.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/PlayerController.h"

// ── Solid brush for SBorder background fills ─────────────────────────────────
// SBorder only renders colour when it has a non-null BorderImage to tint.
// FCoreStyle's "White" brush is a 1×1 white pixel — tinted by BorderBackgroundColor.
inline const FSlateBrush* SolidBrush()
{
	return FCoreStyle::Get().GetBrush("White");
}

// ── Palette (matches btw_ui_kit.html CSS vars) ────────────────────────────────
namespace BTW
{
	static const FLinearColor Bg         = FLinearColor(0.027f, 0.035f, 0.055f, 1.f);  // #07090E
	static const FLinearColor Panel      = FLinearColor(0.027f, 0.035f, 0.055f, 1.f);
	static const FLinearColor Border     = FLinearColor(0.118f, 0.125f, 0.188f, 1.f);    // #1E2030
	static const FLinearColor Border2    = FLinearColor(0.172f, 0.188f, 0.314f, 1.f);    // #2C3050
	static const FLinearColor Gold       = FLinearColor(0.769f, 0.604f, 0.180f, 1.f);    // #C49A2E
	static const FLinearColor GoldDim    = FLinearColor(0.420f, 0.318f, 0.094f, 1.f);    // #6B5118
	static const FLinearColor Cream      = FLinearColor(0.910f, 0.867f, 0.710f, 1.f);    // #E8DDB5
	static const FLinearColor Muted      = FLinearColor(0.353f, 0.322f, 0.251f, 1.f);    // #5A5240
	static const FLinearColor Blue       = FLinearColor(0.122f, 0.431f, 0.749f, 1.f);    // #1F6EBF
	static const FLinearColor BlueLight  = FLinearColor(0.357f, 0.753f, 0.961f, 1.f);    // #5BC0F5
	static const FLinearColor Red        = FLinearColor(0.608f, 0.125f, 0.125f, 1.f);    // #9B2020
	static const FLinearColor RedLight   = FLinearColor(0.875f, 0.439f, 0.439f, 1.f);    // #E07070
	static const FLinearColor Orange     = FLinearColor(0.478f, 0.243f, 0.055f, 1.f);    // #7A3E0E
	static const FLinearColor Green      = FLinearColor(0.165f, 0.420f, 0.227f, 1.f);    // #2A6B3A
	static const FLinearColor GreenLight = FLinearColor(0.365f, 0.749f, 0.471f, 1.f);    // #5DBF78

	// State colours
	static const FLinearColor StateMarch = FLinearColor(0.416f, 0.624f, 0.831f, 1.f);    // #6A9FD4
	static const FLinearColor StateFire  = Gold;
	static const FLinearColor StatePanic = FLinearColor(0.878f, 0.333f, 0.333f, 1.f);    // #E05555
	static const FLinearColor StateRun   = FLinearColor(0.624f, 0.831f, 0.416f, 1.f);    // #9FD46A
	static const FLinearColor StateStand = Muted;
}

/**
 * Battle HUD — Total War style.
 * Visual design matches btw_ui_kit.html.
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

		ChildSlot
		[
			SNew(SOverlay)

			// ══ Layer 1: battlefield HUD — hidden in menu/army screens ══════
			+ SOverlay::Slot()
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([this]() {
					const EGamePhase P = GetGamePhase();
					return (P == EGamePhase::MainMenu || P == EGamePhase::ArmySetup)
						? EVisibility::Collapsed : EVisibility::Visible;
				})

				+ SVerticalBox::Slot().FillHeight(1.f)

				// ── Bottom strip ──────────────────────────────────────────────
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder)
					.BorderImage(SolidBrush())
					.BorderBackgroundColor(BTW::Panel)
					.Padding(0.f)
					[
						SNew(SVerticalBox)

						// Row 1: unit detail (name · state · HP · morale · commands)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SBorder)
							.BorderBackgroundColor(BTW::Border)
							.Padding(FMargin(0.f, 0.f, 0.f, 1.f))
							.Visibility_Lambda([this]() {
								return GetSelectedSpawner() ? EVisibility::Visible : EVisibility::Collapsed;
							})
							[
								SNew(SHorizontalBox)
								.Clipping(EWidgetClipping::ClipToBounds)

								// Name
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 7.f, 10.f, 7.f)
								[
									SNew(STextBlock)
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
									.ColorAndOpacity(FSlateColor(BTW::Cream))
									.Text_Lambda([this]() { return FText::FromString(GetUnitName()); })
								]

								// · State
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
								[
									SNew(STextBlock)
									.Font(FCoreStyle::GetDefaultFontStyle("Italic", 11))
									.ColorAndOpacity_Lambda([this]() -> FSlateColor { return GetStateColor(); })
									.Text_Lambda([this]() -> FText {
										ABattleSpawnerActor* S = GetSelectedSpawner();
										return S ? FText::FromString(TEXT("· ") + S->GetDominantStateString())
										         : FText::GetEmpty();
									})
								]

								// alive / total
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 14.f, 0.f)
								[
									SNew(STextBlock)
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
									.ColorAndOpacity(FSlateColor(BTW::Muted))
									.Text_Lambda([this]() { return FText::FromString(GetHPString()); })
								]

								// Morale block
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 14.f, 0.f)
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot().AutoHeight()
									[
										SNew(STextBlock)
										.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
										.ColorAndOpacity(FSlateColor(BTW::Muted))
										.Text_Lambda([this]() -> FText {
											return FText::FromString(
												FString::Printf(TEXT("Morale %.0f%%"), GetMoralePct() * 100.f));
										})
									]
									+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
									[
										SNew(SBox).WidthOverride(200.f).HeightOverride(8.f)
										[
											SNew(SOverlay)
											+ SOverlay::Slot()
											[ SNew(SBorder).BorderBackgroundColor(FLinearColor(0.04f, 0.04f, 0.07f, 1.f)) ]
											+ SOverlay::Slot().HAlign(HAlign_Left)
											[
												SNew(SBox)
												.WidthOverride_Lambda([this]() -> FOptionalSize {
													return FOptionalSize(GetMoralePct() * 200.f);
												})
												[
													SNew(SBorder)
													.BorderBackgroundColor_Lambda([this]() -> FSlateColor {
														const float M = GetMoralePct();
														if (M > 0.6f) return FLinearColor(0.298f, 0.686f, 0.416f);
														if (M > 0.3f) return BTW::Gold;
														return BTW::Red;
													})
												]
											]
										]
									]
								]

								// Command groups (inline, right side of strip)
								+ SHorizontalBox::Slot().FillWidth(1.f)

								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 4.f, 6.f, 4.f)
								[
									MakeCmdGroup(TEXT("Ruch"), {
										{TEXT("Stać"), 6, true},
										{TEXT("Marsz"),    1, false},
										{TEXT("Bieg"),     2, false},
									})
								]

								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 4.f, 12.f, 4.f)
								[
									MakeCmdGroup(TEXT("Ogień"), {
										{TEXT("Swobodny"), 3, false},
										{TEXT("Salwa"),    4, false},
										{TEXT("Rzędami"), 5, false},
									})
								]
							]
						]

						// Row 2: unit cards strip
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SBox).HeightOverride(86.f)
							[
								SNew(SScrollBox)
								.Orientation(Orient_Horizontal)
								.ScrollBarVisibility(EVisibility::Collapsed)
								+ SScrollBox::Slot()
								[
									SAssignNew(CardsBox, SHorizontalBox)
								]
							]
						]
					]
				]
			]   // end Layer 1

			// ══ Layer 2: end-of-battle card ══════════════════════════════════
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 1.f))
				.HAlign(HAlign_Fill).VAlign(VAlign_Fill)
				.Visibility_Lambda([this]() {
					return GetOutcome() != EBattleOutcome::Ongoing
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					SNew(SBox).WidthOverride(380.f)
					[
						SNew(SBorder)
						.BorderImage(SolidBrush())
					.BorderBackgroundColor(BTW::Panel)
						.Padding(0.f)
						[
							SNew(SVerticalBox)

							// Header: result + flavour
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SBorder)
								.BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.f))
								.Padding(FMargin(30.f, 28.f, 30.f, 22.f))
								.HAlign(HAlign_Center)
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
									[
										SNew(STextBlock)
										.Font(FCoreStyle::GetDefaultFontStyle("Bold", 36))
										.Justification(ETextJustify::Center)
										.ColorAndOpacity_Lambda([this]() -> FSlateColor { return GetOutcomeColor(); })
										.Text_Lambda([this]() -> FText { return FText::FromString(GetOutcomeTitle()); })
									]
									+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 8.f, 0.f, 0.f)
									[
										SNew(STextBlock)
										.Font(FCoreStyle::GetDefaultFontStyle("Italic", 12))
										.Justification(ETextJustify::Center)
										.ColorAndOpacity_Lambda([this]() -> FSlateColor {
											FLinearColor C = GetOutcomeColor().GetSpecifiedColor();
											C.A = 0.7f; return FSlateColor(C);
										})
										.Text_Lambda([this]() -> FText { return FText::FromString(GetOutcomeFlavour()); })
									]
								]
							]

							// Separator
							+ SVerticalBox::Slot().AutoHeight()
							[ SNew(SBorder).BorderBackgroundColor(BTW::Border2).Padding(FMargin(0.f, 1.f, 0.f, 0.f)) ]

							// Stats row
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SHorizontalBox)
								.Clipping(EWidgetClipping::ClipToBounds)

								+ SHorizontalBox::Slot().FillWidth(1.f)
								[ MakeEndStat_Lambda(
									[this]() { return FString::FromInt(GetEnemyCasualties()); },
									TEXT("Straty wroga"),
									BTW::GreenLight) ]

								+ SHorizontalBox::Slot().AutoWidth()
								[ SNew(SBorder).BorderBackgroundColor(BTW::Border2).Padding(FMargin(1.f, 10.f)) ]

								+ SHorizontalBox::Slot().FillWidth(1.f)
								[ MakeEndStat_Lambda(
									[this]() { return FString::FromInt(GetPlayerCasualties()); },
									TEXT("Twoje straty"),
									BTW::RedLight) ]
							]

							// Separator
							+ SVerticalBox::Slot().AutoHeight()
							[ SNew(SBorder).BorderBackgroundColor(BTW::Border2).Padding(FMargin(0.f, 1.f, 0.f, 0.f)) ]

							// Buttons
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SHorizontalBox)
								.Clipping(EWidgetClipping::ClipToBounds)
								+ SHorizontalBox::Slot().FillWidth(1.f).Padding(14.f, 14.f, 6.f, 14.f)
								[
									MakeEndButton(TEXT("JESZCZE RAZ"), true, [this]() { DoRestart(); })
								]
								+ SHorizontalBox::Slot().FillWidth(1.f).Padding(6.f, 14.f, 14.f, 14.f)
								[
									MakeEndButton(TEXT("WYJŚCIE"), false, [this]() { DoQuit(); })
								]
							]
						]
					]
				]
			]

			// ══ Layer 3: time controls (top-center, Battle only) ══════════════
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(0.f, 0.f, 0.f, 0.f)
			[
				SNew(SBorder)
				.BorderBackgroundColor(BTW::Panel)
				.Padding(FMargin(0.f, 0.f, 0.f, 0.f))
				.RenderTransform(FSlateRenderTransform(FVector2D(0.f, 0.f)))
				.Visibility_Lambda([this]() {
					return GetGamePhase() == EGamePhase::Battle
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					SNew(SHorizontalBox)
					// «
					+ SHorizontalBox::Slot().AutoWidth()
					[ MakeTimeBtn(TEXT("«"), [this]() { DoSlower(); }) ]
					// pause/play — wider button
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.ContentPadding(0.f)
						.ButtonColorAndOpacity(FLinearColor::Transparent)
						.OnClicked_Lambda([this]() -> FReply { DoTogglePause(); return FReply::Handled(); })
						[
							SNew(SBorder)
							.Padding(FMargin(16.f, 8.f))
							.HAlign(HAlign_Center).VAlign(VAlign_Center)
							.BorderBackgroundColor_Lambda([this]() -> FSlateColor {
								return BattleSimPaused()
									? FSlateColor(BTW::GoldDim)
									: FSlateColor(BTW::Border2);
							})
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
								.ColorAndOpacity(FSlateColor(BTW::Cream))
								.Text_Lambda([this]() -> FText {
									return FText::FromString(BattleSimPaused() ? TEXT("►") : TEXT("⏸"));
								})
							]
						]
					]
					// »
					+ SHorizontalBox::Slot().AutoWidth()
					[ MakeTimeBtn(TEXT("»"), [this]() { DoFaster(); }) ]
					// speed label
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 10.f, 0.f)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
						.ColorAndOpacity_Lambda([this]() -> FSlateColor {
							return BattleSimPaused()
								? FSlateColor(BTW::Gold)
								: FSlateColor(BTW::BlueLight);
						})
						.Text_Lambda([this]() -> FText {
							return FText::FromString(GetTimeStatusText());
						})
					]
				]
			]

			// ══ Layer 4: main menu ════════════════════════════════════════════
			+ SOverlay::Slot()
			[
				SNew(SBorder)
				.BorderImage(SolidBrush())
				.BorderBackgroundColor(BTW::Bg)
				.HAlign(HAlign_Fill).VAlign(VAlign_Fill)
				.Visibility_Lambda([this]() {
					return GetGamePhase() == EGamePhase::MainMenu
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().FillHeight(1.f)

					// Centre content
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[
						SNew(SBox).WidthOverride(500.f)
						[
							SNew(SVerticalBox)

							// Decorative stripe
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Fill).Padding(0.f, 0.f, 0.f, 20.f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
								[ SNew(SBorder).BorderBackgroundColor(BTW::GoldDim).Padding(FMargin(0.f, 0.5f)) ]
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f)
								[ SNew(STextBlock).Text(FText::FromString(TEXT("◆"))).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8)).ColorAndOpacity(FSlateColor(BTW::GoldDim)) ]
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f, 0.f)
								[ SNew(STextBlock).Text(FText::FromString(TEXT("◆"))).Font(FCoreStyle::GetDefaultFontStyle("Regular", 12)).ColorAndOpacity(FSlateColor(BTW::Gold)) ]
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f, 0.f)
								[ SNew(STextBlock).Text(FText::FromString(TEXT("◆"))).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8)).ColorAndOpacity(FSlateColor(BTW::GoldDim)) ]
								+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
								[ SNew(SBorder).BorderBackgroundColor(BTW::GoldDim).Padding(FMargin(0.f, 0.5f)) ]
							]

							// Eyebrow
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 10.f)
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("Symulator Bitew Napoleońskich")))
								.Font(FCoreStyle::GetDefaultFontStyle("Italic", 11))
								.ColorAndOpacity(FSlateColor(BTW::Muted))
							]

							// Title
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("BIEDA")))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 60))
								.ColorAndOpacity(FSlateColor(BTW::Cream))
								.Justification(ETextJustify::Center)
							]
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("TOTAL WAR")))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 60))
								.ColorAndOpacity(FSlateColor(BTW::Cream))
								.Justification(ETextJustify::Center)
							]

							// Subtitle
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 6.f, 0.f, 0.f)
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("Anno Domini MDCCCXV")))
								.Font(FCoreStyle::GetDefaultFontStyle("Italic", 12))
								.ColorAndOpacity(FSlateColor(BTW::Muted))
							]

							// Divider
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Fill).Padding(0.f, 22.f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
								[ SNew(SBorder).BorderBackgroundColor(BTW::Border2).Padding(FMargin(0.f, 0.5f)) ]
								+ SHorizontalBox::Slot().AutoWidth().Padding(10.f, 0.f).VAlign(VAlign_Center)
								[ SNew(STextBlock).Text(FText::FromString(TEXT("⚔"))).Font(FCoreStyle::GetDefaultFontStyle("Regular", 18)).ColorAndOpacity(FSlateColor(BTW::GoldDim)) ]
								+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
								[ SNew(SBorder).BorderBackgroundColor(BTW::Border2).Padding(FMargin(0.f, 0.5f)) ]
							]

							// Menu items
							+ SVerticalBox::Slot().AutoHeight()
							[ SNew(SBorder).BorderBackgroundColor(BTW::Border).Padding(FMargin(0.f, 0.5f)) ]
							+ SVerticalBox::Slot().AutoHeight()
							[ MakeMenuItem(TEXT("I."), TEXT("GRAJ"), [this]() { DoStartArmySetup(); }) ]
							+ SVerticalBox::Slot().AutoHeight()
							[ SNew(SBorder).BorderBackgroundColor(BTW::Border).Padding(FMargin(0.f, 0.5f)) ]
							+ SVerticalBox::Slot().AutoHeight()
							[ MakeMenuItem(TEXT("II."), TEXT("WYJŚCIE"), [this]() { DoQuit(); }, /*bExit*/true) ]
							+ SVerticalBox::Slot().AutoHeight()
							[ SNew(SBorder).BorderBackgroundColor(BTW::Border).Padding(FMargin(0.f, 0.5f)) ]
						]
					]

					+ SVerticalBox::Slot().FillHeight(1.f)
				]
			]

			// ══ Layer 5: deployment bar ═══════════════════════════════════════
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top)
			[
				SNew(SBorder)
				.BorderBackgroundColor(BTW::Panel)
				.Padding(FMargin(20.f, 8.f))
				.Visibility_Lambda([this]() {
					return GetGamePhase() == EGamePhase::Deploy
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 18.f, 0.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Rozstaw oddziały w strefie (PPM przeciągnij), następnie:")))
						.Font(FCoreStyle::GetDefaultFontStyle("Italic", 12))
						.ColorAndOpacity(FSlateColor(BTW::Muted))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[ MakeGoldButton(TEXT("ROZPOCZNIJ BITWĘ"), [this]() { DoStartBattle(); }) ]
				]
			]

			// ══ Layer 6: army setup ═══════════════════════════════════════════
			+ SOverlay::Slot()
			[
				SNew(SBorder)
				.BorderImage(SolidBrush())
				.BorderBackgroundColor(BTW::Bg)
				.HAlign(HAlign_Fill).VAlign(VAlign_Fill)
				.Visibility_Lambda([this]() {
					return GetGamePhase() == EGamePhase::ArmySetup
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().FillHeight(1.f)

					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 6.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Przed bitwą")))
						.Font(FCoreStyle::GetDefaultFontStyle("Italic", 11))
						.ColorAndOpacity(FSlateColor(BTW::Muted))
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 22.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("WYBÓR ARMII")))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 36))
						.ColorAndOpacity(FSlateColor(BTW::Gold))
					]

					// Two army cards side by side
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 16.f)
					[
						SNew(SBox).WidthOverride(660.f)
						[
							SNew(SHorizontalBox)

							// TWOJA ARMIA (blue)
							+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 7.f, 0.f)
							[
								MakeArmyCard(true)
							]

							// ARMIA WROGA (red)
							+ SHorizontalBox::Slot().FillWidth(1.f).Padding(7.f, 0.f, 0.f, 0.f)
							[
								MakeArmyCard(false)
							]
						]
					]

					// Aggressor toggle
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 20.f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Agresor:")))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
							.ColorAndOpacity(FSlateColor(BTW::Muted))
						]
						// "Wróg" segment
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.ContentPadding(0.f)
							.ButtonColorAndOpacity(FLinearColor::Transparent)
							.OnClicked_Lambda([this]() -> FReply {
								if (ABattleManager* M = GetManager()) M->bEnemyIsAggressor = true;
								return FReply::Handled();
							})
							[
								SNew(SBorder)
								.Padding(FMargin(18.f, 6.f))
								.BorderBackgroundColor_Lambda([this]() -> FSlateColor {
									return EnemyAggressor()
										? FSlateColor(BTW::Red)
										: FSlateColor(BTW::Border2);
								})
								[
									SNew(STextBlock)
									.Text(FText::FromString(TEXT("Wróg")))
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
									.ColorAndOpacity_Lambda([this]() -> FSlateColor {
										return EnemyAggressor() ? FSlateColor(BTW::RedLight) : FSlateColor(BTW::Muted);
									})
								]
							]
						]
						// "Gracz" segment
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.ContentPadding(0.f)
							.ButtonColorAndOpacity(FLinearColor::Transparent)
							.OnClicked_Lambda([this]() -> FReply {
								if (ABattleManager* M = GetManager()) M->bEnemyIsAggressor = false;
								return FReply::Handled();
							})
							[
								SNew(SBorder)
								.Padding(FMargin(18.f, 6.f))
								.BorderBackgroundColor_Lambda([this]() -> FSlateColor {
									return !EnemyAggressor()
										? FSlateColor(BTW::Blue)
										: FSlateColor(BTW::Border2);
								})
								[
									SNew(STextBlock)
									.Text(FText::FromString(TEXT("Gracz")))
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
									.ColorAndOpacity_Lambda([this]() -> FSlateColor {
										return !EnemyAggressor() ? FSlateColor(BTW::BlueLight) : FSlateColor(BTW::Muted);
									})
								]
							]
						]
					]

					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[ MakeGoldButton(TEXT("ROZSTAW"), [this]() { DoConfirmArmies(); }) ]

					+ SVerticalBox::Slot().FillHeight(1.f)
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
	TSharedPtr<SHorizontalBox> CardsBox;
	int32 LastCardCount = -1;

	// ── Getters ───────────────────────────────────────────────────────────

	ABattleCameraPawn* GetCameraPawn() const
	{
		if (!World) return nullptr;
		APlayerController* PC = World->GetFirstPlayerController();
		return PC ? Cast<ABattleCameraPawn>(PC->GetPawn()) : nullptr;
	}

	ABattleSpawnerActor* GetSelectedSpawner() const
	{
		ABattleCameraPawn* Cam = GetCameraPawn();
		return Cam ? Cam->GetSelectedSpawner() : nullptr;
	}

	ABattleManager* GetManager() const
	{
		if (!World) return nullptr;
		for (TActorIterator<ABattleManager> It(World); It; ++It) return *It;
		return nullptr;
	}

	EGamePhase GetGamePhase() const
	{
		ABattleManager* M = GetManager();
		return M ? M->GetGamePhase() : EGamePhase::Battle;
	}

	EBattleOutcome GetOutcome() const
	{
		if (!World) return EBattleOutcome::Ongoing;
		for (TActorIterator<ABattleManager> It(World); It; ++It)
			return It->GetOutcome();
		return EBattleOutcome::Ongoing;
	}

	FString GetUnitName() const
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return TEXT("");
		return (S->UnitType == EUnitType::LineInfantry)
			? TEXT("Piechota Liniowa") : TEXT("Milicja");
	}

	FString GetHPString() const
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return TEXT("");
		return FString::Printf(TEXT("%d / %d"), S->GetAliveCount(), S->NumAgents);
	}

	float GetMoralePct() const
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return 0.f;
		return FMath::Clamp(S->GetAverageMorale() / 100.f, 0.f, 1.f);
	}

	FSlateColor GetStateColor() const
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return FSlateColor(BTW::Muted);
		const FString St = S->GetDominantStateString();
		if (St == TEXT("PANIKA!"))   return FSlateColor(BTW::StatePanic);
		if (St == TEXT("Strzela!"))  return FSlateColor(BTW::StateFire);
		if (St == TEXT("Maszeruje")) return FSlateColor(BTW::StateMarch);
		if (St == TEXT("Biegnie"))   return FSlateColor(BTW::StateRun);
		return FSlateColor(BTW::StateStand);
	}

	FString GetTimeStatusText() const
	{
		if (BattleSimPaused()) return TEXT("PAUZA");
		const float TS = BattleSimTimeScale();
		if (FMath::IsNearlyEqual(TS, 0.25f)) return TEXT("×¼");
		if (FMath::IsNearlyEqual(TS, 0.5f))  return TEXT("×½");
		if (FMath::IsNearlyEqual(TS, 2.f))   return TEXT("×2");
		if (FMath::IsNearlyEqual(TS, 4.f))   return TEXT("×4");
		return TEXT("×1");
	}

	// ── End screen ────────────────────────────────────────────────────────

	FString GetOutcomeTitle() const
	{
		switch (GetOutcome())
		{
		case EBattleOutcome::PlayerVictory: return TEXT("ZWYCIĘSTWO");
		case EBattleOutcome::PlayerDefeat:  return TEXT("PORAŻKA");
		case EBattleOutcome::Draw:          return TEXT("REMIS");
		default:                            return FString();
		}
	}

	FString GetOutcomeFlavour() const
	{
		switch (GetOutcome())
		{
		case EBattleOutcome::PlayerVictory: return TEXT("Chwała armii Jego Królewskiej Mości");
		case EBattleOutcome::PlayerDefeat:  return TEXT("Pole bitwy należy do wroga");
		case EBattleOutcome::Draw:          return TEXT("żadna ze stron nie odniosła zwycięstwa");
		default:                            return FString();
		}
	}

	FSlateColor GetOutcomeColor() const
	{
		switch (GetOutcome())
		{
		case EBattleOutcome::PlayerVictory: return FSlateColor(BTW::GreenLight);
		case EBattleOutcome::PlayerDefeat:  return FSlateColor(BTW::StatePanic);
		default:                            return FSlateColor(BTW::Gold);
		}
	}

	int32 GetPlayerCasualties() const
	{
		int32 Total = 0, Alive = 0;
		for (TActorIterator<ABattleSpawnerActor> It(World); It; ++It)
			if (It->TeamId == 0) { Total += It->NumAgents; Alive += It->GetAliveCount(); }
		return Total - Alive;
	}

	int32 GetEnemyCasualties() const
	{
		int32 Total = 0, Alive = 0;
		for (TActorIterator<ABattleSpawnerActor> It(World); It; ++It)
			if (It->TeamId != 0) { Total += It->NumAgents; Alive += It->GetAliveCount(); }
		return Total - Alive;
	}

	// ── Actions ───────────────────────────────────────────────────────────

	void DoStartBattle()   { if (ABattleManager* M = GetManager()) M->StartBattle(); }
	void DoStartArmySetup(){ if (ABattleManager* M = GetManager()) M->StartArmySetup(); }
	void DoConfirmArmies() { if (ABattleManager* M = GetManager()) M->ConfirmArmiesAndDeploy(); }
	void DoAddSquad(bool bPlayer, EUnitType T)    { if (ABattleManager* M = GetManager()) M->AddSquad(bPlayer, T); }
	void DoRemoveSquad(bool bPlayer, EUnitType T) { if (ABattleManager* M = GetManager()) M->RemoveSquad(bPlayer, T); }
	int32 SquadCount(bool bPlayer, EUnitType T) const { ABattleManager* M = GetManager(); return M ? M->GetSquadCount(bPlayer, T) : 0; }
	bool EnemyAggressor() const { ABattleManager* M = GetManager(); return M ? M->bEnemyIsAggressor : true; }
	int32 SoldiersPerSquad(EUnitType T) const
	{
		ABattleManager* M = GetManager();
		if (!M) return 50;
		return (T == EUnitType::LineInfantry) ? M->LineSoldiersPerSquad : M->MilitiaSoldiersPerSquad;
	}

	void DoRestart()
	{
		if (World)
			UGameplayStatics::OpenLevel(World, FName(*UGameplayStatics::GetCurrentLevelName(World)));
	}

	void DoQuit()
	{
		if (World)
			if (APlayerController* PC = World->GetFirstPlayerController())
				PC->ConsoleCommand(TEXT("quit"));
	}

	void DoTogglePause() { ToggleBattleSimPaused(); }
	void DoSlower()      { StepBattleSimTimeScale(-1); }
	void DoFaster()      { StepBattleSimTimeScale(+1); }

	// ── HUD command helpers ───────────────────────────────────────────────

	struct FCmdEntry { FString Label; int32 Idx; bool bAction; };

	TSharedRef<SWidget> MakeCmdGroup(const FString& GroupLabel, std::initializer_list<FCmdEntry> Entries)
	{
		TSharedRef<SHorizontalBox> BtnRow = SNew(SHorizontalBox);
		for (const FCmdEntry& E : Entries)
		{
			const FString Lbl = E.Label;
			const int32   Idx = E.Idx;
			const bool     bA = E.bAction;
			BtnRow->AddSlot().AutoWidth().Padding(0.f, 0.f, 3.f, 0.f)
			[
				SNew(SButton)
				.ContentPadding(0.f)
				.ButtonColorAndOpacity(FLinearColor::Transparent)
				.OnClicked_Lambda([this, Idx]() -> FReply { ExecuteCommand(Idx); return FReply::Handled(); })
				[
					SNew(SBorder)
					.Padding(FMargin(9.f, 5.f))
					.HAlign(HAlign_Center)
					.BorderBackgroundColor_Lambda([this, Idx, bA]() -> FSlateColor {
						if (bA) return FSlateColor(BTW::Orange);
						return IsCommandActive(Idx)
							? FSlateColor(BTW::Blue)
							: FSlateColor(BTW::Border2);
					})
					[
						SNew(STextBlock)
						.Text(FText::FromString(Lbl))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
						.ColorAndOpacity_Lambda([this, Idx, bA]() -> FSlateColor {
							if (bA) return FSlateColor(FLinearColor(0.831f, 0.533f, 0.227f));
							return IsCommandActive(Idx) ? FSlateColor(BTW::BlueLight) : FSlateColor(BTW::Muted);
						})
					]
				]
			];
		}

		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 3.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(GroupLabel))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor(BTW::Muted))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[ BtnRow ];
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
		case 6: S->IssueHaltOrder(); break;
		}
	}

	bool IsCommandActive(int32 Cmd) const
	{
		ABattleSpawnerActor* S = GetSelectedSpawner();
		if (!S) return false;
		switch (Cmd)
		{
		case 1: return !S->bForceRun;
		case 2: return S->bForceRun;
		case 3: return S->VolleyMode == EVolleyMode::FreeFire;
		case 4: return S->VolleyMode == EVolleyMode::SquadVolley;
		case 5: return S->VolleyMode == EVolleyMode::RankFire;
		default: return false;
		}
	}

	// ── Unit cards ────────────────────────────────────────────────────────

	void RebuildCards()
	{
		if (!CardsBox.IsValid() || !World) return;

		TArray<ABattleSpawnerActor*> Friendly;
		for (TActorIterator<ABattleSpawnerActor> It(World); It; ++It)
			if (It->TeamId == 0) Friendly.Add(*It);

		if (Friendly.Num() == LastCardCount) return;
		LastCardCount = Friendly.Num();
		CardsBox->ClearChildren();

		for (int32 i = 0; i < Friendly.Num(); ++i)
		{
			ABattleSpawnerActor* Spawner = Friendly[i];
			const int32 Num = i + 1;

			CardsBox->AddSlot().AutoWidth().Padding(3.f, 6.f, 0.f, 6.f)
			[
				SNew(SButton)
				.ContentPadding(0.f)
				.ButtonColorAndOpacity(FLinearColor::Transparent)
				.OnClicked_Lambda([this, Spawner]() -> FReply {
					ABattleCameraPawn* Cam = GetCameraPawn();
					if (Cam) Cam->SetSelectedSpawner(Spawner);
					return FReply::Handled();
				})
				[
					SNew(SBox).WidthOverride(70.f).HeightOverride(74.f)
					[
						SNew(SBorder)
						.Padding(4.f)
						.BorderBackgroundColor_Lambda([this, Spawner]() -> FSlateColor {
							const FString St = Spawner->GetDominantStateString();
							const bool bRout = (St == TEXT("PANIKA!"));
							if (GetSelectedSpawner() == Spawner)
								return FSlateColor(BTW::Blue);
							if (bRout)
								return FSlateColor(BTW::Red);
							return FSlateColor(FLinearColor(0.04f, 0.04f, 0.07f, 1.f));
						})
						[
							SNew(SVerticalBox)

							// Number
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(BTW::Muted))
								.Text(FText::FromString(FString::Printf(TEXT("%d."), Num)))
							]

							// Alive count (big)
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 20))
								.ColorAndOpacity(FSlateColor(BTW::Cream))
								.Text_Lambda([Spawner]() -> FText {
									return FText::FromString(FString::FromInt(Spawner->GetAliveCount()));
								})
							]

							// / total
							+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(BTW::Muted))
								.Text(FText::FromString(FString::Printf(TEXT("/ %d"), Spawner->NumAgents)))
							]

							+ SVerticalBox::Slot().FillHeight(1.f)

							// Morale bar
							+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
							[
								SNew(SBox).HeightOverride(5.f)
								[
									SNew(SOverlay)
									+ SOverlay::Slot()
									[ SNew(SBorder).BorderBackgroundColor(FLinearColor(0.04f, 0.04f, 0.07f)) ]
									+ SOverlay::Slot().HAlign(HAlign_Left)
									[
										SNew(SBox)
										.WidthOverride_Lambda([Spawner]() -> FOptionalSize {
											return FOptionalSize(FMath::Clamp(Spawner->GetAverageMorale() / 100.f, 0.f, 1.f) * 62.f);
										})
										[
											SNew(SBorder)
											.BorderBackgroundColor_Lambda([Spawner]() -> FSlateColor {
												const float M = Spawner->GetAverageMorale();
												if (M > 60.f) return FSlateColor(FLinearColor(0.298f, 0.686f, 0.416f));
												if (M > 30.f) return FSlateColor(BTW::Gold);
												return FSlateColor(BTW::Red);
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

	// ── Widget factories ──────────────────────────────────────────────────

	TSharedRef<SWidget> MakeMenuItem(const FString& Num, const FString& Label,
		TFunction<void()> OnClick, bool bExit = false)
	{
		return SNew(SButton)
			.ContentPadding(0.f)
			.ButtonColorAndOpacity(FLinearColor::Transparent)
			.OnClicked_Lambda([OnClick]() -> FReply { if (OnClick) OnClick(); return FReply::Handled(); })
			[
				SNew(SBorder)
				.Padding(FMargin(18.f, 14.f))
				.BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 16.f, 0.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Num))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
						.ColorAndOpacity(FSlateColor(bExit ? BTW::Red : BTW::GoldDim))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Label))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 22))
						.ColorAndOpacity(FSlateColor(bExit ? BTW::RedLight : BTW::Cream))
					]
				]
			];
	}

	TSharedRef<SWidget> MakeGoldButton(const FString& Label, TFunction<void()> OnClick)
	{
		return SNew(SButton)
			.ContentPadding(0.f)
			.ButtonColorAndOpacity(FLinearColor::Transparent)
			.OnClicked_Lambda([OnClick]() -> FReply { if (OnClick) OnClick(); return FReply::Handled(); })
			[
				SNew(SBorder)
				.Padding(FMargin(38.f, 12.f))
				.HAlign(HAlign_Center)
				.BorderBackgroundColor(FLinearColor(0.12f, 0.10f, 0.04f, 1.f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 15))
					.ColorAndOpacity(FSlateColor(BTW::Gold))
				]
			];
	}

	TSharedRef<SWidget> MakeTimeBtn(const FString& Sym, TFunction<void()> OnClick)
	{
		return SNew(SButton)
			.ContentPadding(0.f)
			.ButtonColorAndOpacity(FLinearColor::Transparent)
			.OnClicked_Lambda([OnClick]() -> FReply { if (OnClick) OnClick(); return FReply::Handled(); })
			[
				SNew(SBorder).Padding(FMargin(12.f, 7.f)).HAlign(HAlign_Center)
				.BorderBackgroundColor(BTW::Border2)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Sym))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
					.ColorAndOpacity(FSlateColor(BTW::Cream))
				]
			];
	}

	TSharedRef<SWidget> MakeEndButton(const FString& Label, bool bPrimary, TFunction<void()> OnClick)
	{
		return SNew(SButton)
			.ContentPadding(0.f)
			.ButtonColorAndOpacity(FLinearColor::Transparent)
			.OnClicked_Lambda([OnClick]() -> FReply { if (OnClick) OnClick(); return FReply::Handled(); })
			[
				SNew(SBorder)
				.Padding(FMargin(0.f, 10.f))
				.HAlign(HAlign_Center)
				.BorderBackgroundColor(bPrimary
					? FLinearColor(0.122f, 0.431f, 0.749f, 1.f)
					: FLinearColor(0.06f, 0.06f, 0.10f, 1.f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					.ColorAndOpacity(FSlateColor(bPrimary ? BTW::BlueLight : BTW::Cream))
				]
			];
	}

	TSharedRef<SWidget> MakeEndStat_Lambda(TFunction<FString()> ValFn,
		const FString& StatLabel, const FLinearColor& ValColor)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 12.f, 0.f, 2.f)
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 22))
				.ColorAndOpacity(FSlateColor(ValColor))
				.Text_Lambda([ValFn]() -> FText { return FText::FromString(ValFn()); })
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 10.f)
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(BTW::Muted))
				.Text(FText::FromString(StatLabel))
			];
	}

	TSharedRef<SWidget> MakeArmyCard(bool bPlayer)
	{
		const FLinearColor HeaderBg  = bPlayer
			? FLinearColor(0.122f, 0.431f, 0.749f, 0.14f)
			: FLinearColor(0.608f, 0.125f, 0.125f, 0.14f);
		const FLinearColor TitleCol  = bPlayer ? BTW::BlueLight : BTW::RedLight;
		const FString      Title     = bPlayer ? TEXT("Twoja Armia") : TEXT("Armia Wroga");
		const FLinearColor TotalCol  = TitleCol;

		return SNew(SBorder)
			.BorderBackgroundColor(BTW::Panel)
			.Padding(0.f)
			[
				SNew(SVerticalBox)

				// Header
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder)
					.BorderBackgroundColor(HeaderBg)
					.Padding(FMargin(12.f, 8.f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Title))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
							.ColorAndOpacity(FSlateColor(TitleCol))
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
							.ColorAndOpacity(FSlateColor(FLinearColor(TitleCol.R, TitleCol.G, TitleCol.B, 0.7f)))
							.Text_Lambda([this, bPlayer]() -> FText {
								const int32 Total = (SquadCount(bPlayer, EUnitType::Militia)
									* SoldiersPerSquad(EUnitType::Militia))
									+ (SquadCount(bPlayer, EUnitType::LineInfantry) * SoldiersPerSquad(EUnitType::LineInfantry));
								return FText::FromString(FString::Printf(TEXT("%d żołnierzy"), Total));
							})
						]
					]
				]

				// Body
				+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 10.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
					[ MakeUnitRow(TEXT("Milicja"), TEXT("150 żołnierzy / oddział \xB7 niskie morale"),
						bPlayer, EUnitType::Militia) ]
					+ SVerticalBox::Slot().AutoHeight()
					[ MakeUnitRow(TEXT("Piechota Liniowa"), TEXT("120 żołnierzy / oddział \xB7 salwy rangowe"),
						bPlayer, EUnitType::LineInfantry) ]
				]

				// Total row
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder)
					.BorderBackgroundColor(BTW::Border2)
					.Padding(FMargin(12.f, 6.f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Łącznie")))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
							.ColorAndOpacity(FSlateColor(BTW::Muted))
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
							.ColorAndOpacity(FSlateColor(TotalCol))
							.Text_Lambda([this, bPlayer]() -> FText {
								const int32 Total = (SquadCount(bPlayer, EUnitType::Militia)
									* SoldiersPerSquad(EUnitType::Militia))
									+ (SquadCount(bPlayer, EUnitType::LineInfantry) * SoldiersPerSquad(EUnitType::LineInfantry));
								return FText::FromString(FString::FromInt(Total));
							})
						]
					]
				]
			];
	}

	TSharedRef<SWidget> MakeUnitRow(const FString& Name, const FString& Hint,
		bool bPlayer, EUnitType T)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Name))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
					.ColorAndOpacity(FSlateColor(BTW::Cream))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Hint))
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
					.ColorAndOpacity(FSlateColor(BTW::Muted))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
				[ MakeCounterBtn(TEXT("−"), bPlayer, T, false) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f)
				[
					SNew(SBox).WidthOverride(22.f).HAlign(HAlign_Center)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
						.ColorAndOpacity(FSlateColor(BTW::Cream))
						.Text_Lambda([this, bPlayer, T]() -> FText {
							return FText::FromString(FString::FromInt(SquadCount(bPlayer, T)));
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[ MakeCounterBtn(TEXT("+"), bPlayer, T, true) ]
			];
	}

	TSharedRef<SWidget> MakeCounterBtn(const FString& Sym, bool bPlayer, EUnitType T, bool bAdd)
	{
		return SNew(SButton)
			.ContentPadding(0.f)
			.ButtonColorAndOpacity(FLinearColor::Transparent)
			.OnClicked_Lambda([this, bPlayer, T, bAdd]() -> FReply {
				if (bAdd) DoAddSquad(bPlayer, T); else DoRemoveSquad(bPlayer, T);
				return FReply::Handled();
			})
			[
				SNew(SBox).WidthOverride(26.f).HeightOverride(26.f)
				[
					SNew(SBorder)
					.HAlign(HAlign_Center).VAlign(VAlign_Center)
					.BorderBackgroundColor(BTW::Border2)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Sym))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
						.ColorAndOpacity(FSlateColor(BTW::Cream))
					]
				]
			];
	}
};
