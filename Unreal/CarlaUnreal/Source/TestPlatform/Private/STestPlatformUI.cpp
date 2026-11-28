#include "STestPlatformUI.h"

#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "TestPlatformUI"

DEFINE_LOG_CATEGORY_STATIC(LogTestPlatformUI, Log, All);

namespace
{
	// ---- palette (mirrors the HTML prototype)
	const FLinearColor ColShell(0.027f, 0.035f, 0.047f, 0.985f);
	const FLinearColor ColPanel(0.043f, 0.055f, 0.070f, 0.985f);
	const FLinearColor ColCard(0.055f, 0.070f, 0.086f, 1.f);
	const FLinearColor ColCardHover(0.075f, 0.098f, 0.120f, 1.f);
	const FLinearColor ColCardSel(0.065f, 0.180f, 0.155f, 1.f);
	const FLinearColor ColLine(0.094f, 0.125f, 0.153f, 1.f);
	const FLinearColor ColAccent(0.133f, 0.827f, 0.651f, 1.f);
	const FLinearColor ColWarn(1.0f, 0.706f, 0.329f, 1.f);
	const FLinearColor ColText(0.902f, 0.937f, 0.961f, 1.f);
	const FLinearColor ColMuted(0.549f, 0.620f, 0.678f, 1.f);
	const FLinearColor ColDim(0.392f, 0.459f, 0.522f, 1.f);
	const FLinearColor ColInk(0.020f, 0.055f, 0.047f, 1.f);

	const FSlateBrush& Brush(const FLinearColor& Color)
	{
		static TMap<FString, TSharedPtr<FSlateColorBrush>> Cache;
		const FString Key = Color.ToString();
		if (TSharedPtr<FSlateColorBrush>* Found = Cache.Find(Key))
		{
			return *Found->Get();
		}
		TSharedPtr<FSlateColorBrush> Created = MakeShared<FSlateColorBrush>(Color);
		Cache.Add(Key, Created);
		return *Created.Get();
	}

	const FButtonStyle& StyleFlat(const FLinearColor& Normal, const FLinearColor& Hovered,
		const FLinearColor& Pressed)
	{
		FButtonStyle Style;
		Style.SetNormal(Brush(Normal));
		Style.SetHovered(Brush(Hovered));
		Style.SetPressed(Brush(Pressed));
		Style.SetNormalPadding(FMargin(0.f));
		Style.SetPressedPadding(FMargin(0.f));
		return *new FButtonStyle(Style); // static-lifetime, one per call site below
	}

	const FButtonStyle& BtnChipIdle()
	{
		static FButtonStyle Style = StyleFlat(ColCard, ColCardHover, ColCardHover);
		return Style;
	}

	const FButtonStyle& BtnChipOn()
	{
		static FButtonStyle Style = StyleFlat(FLinearColor(0.086f, 0.318f, 0.267f, 1.f),
			FLinearColor(0.110f, 0.400f, 0.337f, 1.f),
			FLinearColor(0.086f, 0.318f, 0.267f, 1.f));
		return Style;
	}

	const FButtonStyle& BtnGhost()
	{
		static FButtonStyle Style = StyleFlat(FLinearColor(0.100f, 0.130f, 0.160f, 1.f),
			FLinearColor(0.140f, 0.180f, 0.220f, 1.f),
			FLinearColor(0.080f, 0.110f, 0.140f, 1.f));
		return Style;
	}

	const FButtonStyle& BtnPrimary()
	{
		static FButtonStyle Style = StyleFlat(FLinearColor(0.075f, 0.700f, 0.560f, 1.f),
			FLinearColor(0.110f, 0.800f, 0.640f, 1.f),
			FLinearColor(0.060f, 0.580f, 0.470f, 1.f));
		return Style;
	}

	const FButtonStyle& BtnCard()
	{
		static FButtonStyle Style = StyleFlat(ColCard, ColCardHover, ColCardHover);
		return Style;
	}

	/** UI scale: the frame is read from a normal viewing distance, so every
	 *  size goes up ~28% from the prototype's values. */
	constexpr float TypeScale = 1.28f;

	FSlateFontInfo F(int32 Size = 11, bool bBold = false)
	{
		const int32 Scaled = FMath::RoundToInt(Size * TypeScale);
		return FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), Scaled);
	}

	TSharedRef<SWidget> Text(const FString& Value, int32 Size = 11, bool bBold = false,
		const FLinearColor& Color = ColText)
	{
		return SNew(STextBlock)
			.Text(FText::FromString(Value))
			.Font(F(Size, bBold))
			.ColorAndOpacity(FSlateColor(Color));
	}

	/** Width of the right-hand column. The wrapping hints derive their wrap
	 *  width from it, so resizing the column cannot silently re-clip them. */
	constexpr float RightColumnWidth = 384.f;
	/** SBorder padding inside that column, above the scroll box. */
	constexpr float RightColumnPadding = 10.f;
	/** One Card() nests two SBorder layers of the same padding. */
	constexpr float CardPadding = 12.f;

	/** Text() plus wrapping at a fixed width. Only the long hint paragraphs use
	 *  this: Text() stays single-line, so enabling wrap here cannot reflow the
	 *  rest of the panel. AutoWrapText additionally honours a narrower
	 *  allotment, which keeps the text inside the column if the scroll box ever
	 *  grows a scrollbar. */
	TSharedRef<SWidget> WrappedText(const FString& Value, float WrapWidth, int32 Size = 11,
		bool bBold = false, const FLinearColor& Color = ColText)
	{
		return SNew(STextBlock)
			.Text(FText::FromString(Value))
			.Font(F(Size, bBold))
			.ColorAndOpacity(FSlateColor(Color))
			.WrapTextAt(WrapWidth)
			.AutoWrapText(true);
	}

	/** Content width of a section directly inside the right column. */
	constexpr float RightColumnContentWidth = RightColumnWidth - 2.f * RightColumnPadding;
	/** Content width inside one Card() of that column. */
	constexpr float RightCardContentWidth = RightColumnContentWidth - 2.f * CardPadding;


	/** Decimal places a step implies: 5 -> 0, 0.5 -> 1, 0.05 -> 2. */
	int32 DecimalsForStep(float Step)
	{
		if (Step >= 1.f)
		{
			return 0;
		}
		if (Step >= 0.1f)
		{
			return 1;
		}
		if (Step >= 0.01f)
		{
			return 2;
		}
		return 3;
	}

	/** "50 km/h" / "2.0 m" / "3" — the value as the schema's unit demands. */
	FString FormatParamNumber(float Value, float Step, const FString& Unit)
	{
		const FString Number = FString::Printf(TEXT("%.*f"), DecimalsForStep(Step), Value);
		return Unit.IsEmpty() ? Number : FString::Printf(TEXT("%s %s"), *Number, *Unit);
	}

	TSharedRef<SWidget> Spacer(float Amount = 1.f)
	{
		return SNew(SSpacer).Size(FVector2D(Amount, 1.f));
	}
}

// ---------------------------------------------------------------------- //
// construct
// ---------------------------------------------------------------------- //
void STestPlatformUI::Construct(const FArguments& InArgs)
{
	Api = MakeShared<FTestPlatformApi>();
	ConstructedAt = FPlatformTime::Seconds();
	LastCatalogAttemptAt = ConstructedAt;
	// First attempt: usually fails when the backend is not up yet. TickRefresh
	// retries it as soon as /health starts answering, so the panel adopts a
	// backend it did not have at construction (see the retry in TickRefresh).
	Api->LoadCatalog();

	ChildSlot
	[
		SNew(SOverlay)
		.Visibility(EVisibility::SelfHitTestInvisible)
		+ SOverlay::Slot()
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this]()
			{
				return bUIVisible ? EVisibility::Visible : EVisibility::Collapsed;
			})
			+ SVerticalBox::Slot().AutoHeight()[MakeTopBar()]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox).WidthOverride(300.f)[MakeLeftColumn()]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				[
					MakeCenterOverlays()
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox).WidthOverride(RightColumnWidth)[MakeRightColumn()]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()[MakeBottomBar()]
		]
		// loading overlay
		+ SOverlay::Slot()
		[
			SAssignNew(LoadingBox, SBox)
			.Visibility(EVisibility::Collapsed)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				MakeLoadingOverlay()
			]
		]
		// backend offline / starting / waiting to be adopted
		+ SOverlay::Slot()
		[
			SNew(SBox)
			.Visibility_Lambda([this]() { return OfflineCardVisibility(); })
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				Card(SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return OfflineTitle(); })
						.Font(F(14, true)).ColorAndOpacity(FSlateColor(ColWarn))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return OfflineHint(); })
						.Font(F(11)).ColorAndOpacity(FSlateColor(ColMuted))
						.WrapTextAt(600.f).AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					.Padding(FMargin(0.f, 8.f, 0.f, 0.f))
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return FText::FromString(BackendStartError); })
						.Font(F(10, true)).ColorAndOpacity(FSlateColor(ColWarn))
						.Visibility_Lambda([this]() { return OfflineErrorVisibility(); })
						.WrapTextAt(600.f).AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					.Padding(FMargin(0.f, 12.f, 0.f, 0.f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.f, 0.f, 8.f, 0.f))
						[
							SNew(SButton)
							.ButtonStyle(&BtnPrimary())
							.ContentPadding(FMargin(18.f, 6.f))
							.Visibility_Lambda([this]() { return StartBackendButtonVisibility(); })
							.IsEnabled_Lambda([this]() { return IsStartBackendEnabled(); })
							.OnClicked_Lambda([this]()
							{
								RequestBackendStart();
								return FReply::Handled();
							})
							[
								SNew(STextBlock)
								.Text_Lambda([this]() { return StartBackendLabel(); })
								.Font(F(11, true)).ColorAndOpacity(FSlateColor(ColInk))
							]
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.ButtonStyle(&BtnGhost())
							.ContentPadding(FMargin(16.f, 6.f))
							.OnClicked_Lambda([this]() { ReloadCatalog(); return FReply::Handled(); })
							[Text(LOCTEXT("Retry", "重试").ToString(), 11)]
						]
					])
			]
		]
	];

	RegisterActiveTimer(0.25f,
		FWidgetActiveTimerDelegate::CreateSP(this, &STestPlatformUI::TickRefresh));
	SetStatus(LOCTEXT("Ready", "就绪：选地图与参数，然后进入 CARLA 世界").ToString());
	UpdateChrome();
}

TSharedRef<SWidget> STestPlatformUI::Card(TSharedRef<SWidget> Body)
{
	return SNew(SBorder)
		.BorderImage(&Brush(ColPanel))
		.Padding(FMargin(12.f, 10.f))
		[
			SNew(SBorder)
			.BorderImage(&Brush(ColCard))
			.Padding(FMargin(12.f, 10.f))
			[
				Body
			]
		];
}

TSharedRef<SWidget> STestPlatformUI::NumberedTitle(const FString& Number, const FString& Title,
	const FString& Aside)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBorder)
			.BorderImage(&Brush(FLinearColor(0.086f, 0.318f, 0.267f, 1.f)))
			.Padding(FMargin(5.f, 1.f))
			[
				Text(Number, 10, true, ColAccent)
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(7.f, 0.f, 0.f, 0.f))
		[
			Text(Title, 13, true)
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)[Spacer()]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			Text(Aside, 10, false, ColDim)
		];
}

// ---------------------------------------------------------------------- //
// top bar
// ---------------------------------------------------------------------- //
TSharedRef<SWidget> STestPlatformUI::MakeTopBar()
{
	return SNew(SBorder)
		.BorderImage(&Brush(ColShell))
		.Padding(FMargin(14.f, 8.f))
		[
			SNew(SHorizontalBox)
			// brand
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderImage(&Brush(FLinearColor(0.100f, 0.420f, 0.350f, 1.f)))
				.Padding(FMargin(7.f, 4.f))
				[Text(TEXT("AA"), 11, true, ColInk)]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(9.f, 0.f, 0.f, 0.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[Text(TEXT("CARLA 自动驾驶测试平台"), 14, true)]
				+ SVerticalBox::Slot().AutoHeight()[Text(TEXT("SCENARIO CONFIGURATOR · 运行时"), 9, false, ColDim)]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)[Spacer()]
			// status chips
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 8.f, 0.f))
			[
				SNew(SBorder).BorderImage(&Brush(ColCard)).Padding(FMargin(8.f, 4.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[SAssignNew(ServerDot, SBorder).BorderImage(&Brush(ColDim)).Padding(FMargin(3.f, 3.f))]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 0.f, 0.f))
					[
						// no address is known before the first /health answer, and
						// inventing one would be a lie the state no longer covers
						SAssignNew(ServerChipText, STextBlock).Text(FText::FromString(TEXT("服务器 — · 未知")))
						.Font(F(10)).ColorAndOpacity(FSlateColor(ColMuted))
					]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 8.f, 0.f))
			[
				SAssignNew(SessionChipText, STextBlock).Text(FText::FromString(TEXT("会话 · 未开始")))
				.Font(F(10, true)).ColorAndOpacity(FSlateColor(ColMuted))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 12.f, 0.f))
			[
				SNew(SBorder).BorderImage(&Brush(ColCard)).Padding(FMargin(8.f, 4.f))
				[
					// Painted by UpdateChrome from the backend's own report of the
					// clock (sync_mode / fixed_delta / sync_lost). It used to be a
					// constant "20 Hz", which outlived the session it described.
					SAssignNew(SyncChipText, STextBlock).Text(FText::FromString(TEXT("同步模式 · —")))
					.Font(F(10)).ColorAndOpacity(FSlateColor(ColDim))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 8.f, 0.f))
			[
				SNew(SButton).ButtonStyle(&BtnGhost()).ContentPadding(FMargin(14.f, 6.f))
				.OnClicked_Lambda([this]() { ReloadCatalog(); return FReply::Handled(); })
				[Text(LOCTEXT("Preset", "载入预设").ToString(), 11)]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton).ButtonStyle(&BtnPrimary()).ContentPadding(FMargin(16.f, 7.f))
				.OnClicked_Lambda([this]() { OnPrimaryClicked(); return FReply::Handled(); })
				[
					SAssignNew(PrimaryTopLabel, STextBlock)
					.Text(LOCTEXT("Enter", "进入 CARLA 世界 →"))
					.Font(F(11, true)).ColorAndOpacity(FSlateColor(ColInk))
				]
			]
		];
}

// ---------------------------------------------------------------------- //
// left column: maps
// ---------------------------------------------------------------------- //
TSharedRef<SWidget> STestPlatformUI::MakeLeftColumn()
{
	return SNew(SBorder)
		.BorderImage(&Brush(ColPanel))
		.Padding(FMargin(10.f, 10.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBorder).BorderImage(&Brush(ColCard)).Padding(FMargin(8.f, 4.f))
				[
					SAssignNew(SearchBox, SEditableTextBox)
					.HintText(LOCTEXT("Search", "搜索地图 / 标签…"))
					.Font(F(11))
					.OnTextChanged_Lambda([this](const FText& Value)
					{
						SearchFilter = Value.ToString();
						RebuildMapRows();
					})
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.f).Padding(FMargin(0.f, 8.f, 0.f, 0.f))
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(MapRowsBox, SVerticalBox)
				]
			]
		];
}

TSharedRef<SWidget> STestPlatformUI::MapCard(const FMapInfo& Map, bool bSelected,
	TFunction<void()> OnClicked)
{
	const FString Stats = FString::Printf(TEXT("%d 条道路 · %d 个路口"), Map.Roads, Map.Junctions);
	const FString Presets = FString::Printf(TEXT("%d 条预设 · %d"),
		Map.Presets.Num(), Map.Roads);

	const FString ThumbUrl = FString::Printf(TEXT("/maps/%s/thumb.png?w=272&h=84&preset=%s"),
		*Map.Id,
		Map.Presets.Num() > 0 ? *Map.Presets[0].Id : TEXT("loop"));
	TSharedRef<SWidget> Art = SNew(SBox).HeightOverride(84.f);
	if (const FSlateBrush* BrushPtr = Thumb(ThumbUrl, FVector2D(272.f, 84.f)))
	{
		Art = SNew(SBox).HeightOverride(84.f)
			[
				SNew(SImage).Image(BrushPtr)
			];
	}

	return SNew(SButton)
		.ButtonStyle(bSelected ? &BtnChipOn() : &BtnCard())
		.ContentPadding(FMargin(8.f, 7.f))
		.OnClicked_Lambda([OnClicked]()
		{
			if (OnClicked)
			{
				OnClicked();
			}
			return FReply::Handled();
		})
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[Art]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 6.f, 0.f, 0.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBorder)
					.BorderImage(&Brush(bSelected ? ColAccent : ColDim))
					.Padding(FMargin(2.f, 2.f))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 0.f, 0.f))
				[Text(Map.Name, 12, true)]
			]
			+ SVerticalBox::Slot().AutoHeight()[Text(Stats, 9, false, ColDim)]
			+ SVerticalBox::Slot().AutoHeight()[Text(Presets, 9, false, ColDim)]
		];
}

void STestPlatformUI::RebuildMapRows()
{
	if (!MapRowsBox.IsValid() || !Api.IsValid())
	{
		return;
	}
	MapRowsBox->ClearChildren();

	// header row: "1 地图" + count
	MapRowsBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 8.f))
	[
		NumberedTitle(TEXT("1"), LOCTEXT("Maps", "地图").ToString(),
			FString::Printf(TEXT("%d 张可用"), Api->Maps().Num()))
	];

	const FString Filter = SearchFilter.ToLower();
	for (const FMapInfo& Map : Api->Maps())
	{
		if (!Filter.IsEmpty() && !Map.Id.ToLower().Contains(Filter))
		{
			continue;
		}
		const FString MapId = Map.Id;
		const bool bSelected = (MapId == Config.MapId);
		MapRowsBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 6.f))
		[
			MapCard(Map, bSelected, [this, MapId]() { OnSelectMap(MapId); })
		];
	}
}

// ---------------------------------------------------------------------- //
// centre: chips, world (empty), legend, ego/weather cards
// ---------------------------------------------------------------------- //
TSharedRef<SWidget> STestPlatformUI::MakeCenterOverlays()
{
	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox)
		.Visibility(EVisibility::SelfHitTestInvisible);

	// row 1: current selection chips
	Column->AddSlot().AutoHeight().Padding(FMargin(12.f, 8.f, 12.f, 0.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 6.f, 0.f))
		[
			SNew(SBorder).BorderImage(&Brush(ColShell)).Padding(FMargin(8.f, 3.f))
			[
				SAssignNew(MapChipText, STextBlock).Text(FText::FromString(TEXT("未选地图")))
				.Font(F(10, true)).ColorAndOpacity(FSlateColor(ColAccent))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 6.f, 0.f))
		[
			SNew(SBorder).BorderImage(&Brush(ColShell)).Padding(FMargin(8.f, 3.f))
			[
				SAssignNew(WeatherChipText, STextBlock).Text(FText::FromString(TEXT("天气")))
				.Font(F(10)).ColorAndOpacity(FSlateColor(ColMuted))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)[Spacer()]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SAssignNew(CaptionText, STextBlock).Text(FText::GetEmpty())
			.Font(F(10)).ColorAndOpacity(FSlateColor(ColDim))
		]
	];

	// row 2: route presets
	Column->AddSlot().AutoHeight().Padding(FMargin(12.f, 8.f, 12.f, 0.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 8.f, 0.f))
		[
			NumberedTitle(TEXT("3"), LOCTEXT("RouteTitle", "主车路线").ToString())
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SAssignNew(RouteChipsBox, SVerticalBox)
		]
	];

	// row 3: the world. A transparent catcher handles drag/zoom for the overview camera.
	Column->AddSlot().FillHeight(1.f)[MakeViewportCatcher()];

	// row 4: legend + view mode
	Column->AddSlot().AutoHeight().Padding(FMargin(12.f, 0.f, 12.f, 6.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[SNew(SBorder).BorderImage(&Brush(ColShell)).Padding(FMargin(8.f, 3.f))
			[Text(LOCTEXT("Legend", "可行驾驶道路 · 规划路线 · 主车 · 水域").ToString(), 9, false, ColDim)]]
		+ SHorizontalBox::Slot().FillWidth(1.f)[Spacer()]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[SNew(SBorder).BorderImage(&Brush(ColShell)).Padding(FMargin(8.f, 3.f))
			[Text(LOCTEXT("View", "视角：俯视 / 跟随 / 驾驶员").ToString(), 9, false, ColDim)]]
	];

	// row 5: ego + weather cards side by side
	Column->AddSlot().AutoHeight().Padding(FMargin(12.f, 0.f, 12.f, 10.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).Padding(FMargin(0.f, 0.f, 6.f, 0.f))
		[
			Card(SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					NumberedTitle(TEXT("2"), LOCTEXT("EgoTitle", "主车类型").ToString(), TEXT("vehicle.*"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
				[
					SAssignNew(VehicleGrid, SWrapBox).UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(6.f, 6.f))
				])
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			Card(SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					NumberedTitle(TEXT("4"), LOCTEXT("WeatherTitle", "天气").ToString(),
						LOCTEXT("WeatherAside", "CARLA 预设").ToString())
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
				[
					SAssignNew(WeatherGrid, SWrapBox).UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(6.f, 6.f))
				])
		]
	];

	return Column;
}

namespace
{
	/** A hero-select style card: still frame at rest, spins and grows on hover. */
	class SVehicleCard : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SVehicleCard)
			: _Frames(0), _FrameWidth(208.f), _FrameHeight(117.f), _Selected(false)
		{}
			SLATE_ARGUMENT(FString, Name)
			SLATE_ARGUMENT(FString, ClassName)
			SLATE_ARGUMENT(FString, UrlPrefix)
			SLATE_ARGUMENT(int32, Frames)
			SLATE_ARGUMENT(float, FrameWidth)
			SLATE_ARGUMENT(float, FrameHeight)
			SLATE_ARGUMENT(bool, Selected)
			SLATE_EVENT(FSimpleDelegate, OnClicked)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Name = InArgs._Name;
			ClassName = InArgs._ClassName;
			UrlPrefix = InArgs._UrlPrefix;
			Frames = InArgs._Frames;
			FrameSize = FVector2D(InArgs._FrameWidth, InArgs._FrameHeight);
			bSelected = InArgs._Selected;
			ClickHandler = InArgs._OnClicked;

			ChildSlot
			[
				SAssignNew(ScaleBox, SScaleBox)
				.Stretch(EStretch::ScaleToFit)
				.StretchDirection(EStretchDirection::Both)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox)
						.WidthOverride(FrameSize.X)
						.HeightOverride(FrameSize.Y)
						[
							SAssignNew(ImageWidget, SImage)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 0.f))
					[
						SNew(STextBlock).Font(F(10, true)).Text(FText::FromString(Name))
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Font(F(9)).Text(FText::FromString(ClassName))
						.ColorAndOpacity(FSlateColor(ColDim))
					]
				]
			];
			RefreshImage();
		}

		void SetBrushProvider(TFunction<const FSlateBrush*(const FString&, FVector2D)> InProvider)
		{
			BrushProvider = InProvider;
			PrefetchAll();
			RefreshImage();
		}

		virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
		{
			SCompoundWidget::OnMouseEnter(MyGeometry, MouseEvent);
			bHovered = true;
			if (Frames > 1 && !TimerHandle.IsValid())
			{
				TimerHandle = RegisterActiveTimer(1.0f / 12.0f,
					FWidgetActiveTimerDelegate::CreateSP(this, &SVehicleCard::Animate));
			}
			if (ScaleBox.IsValid())
			{
				ScaleBox->SetUserSpecifiedScale(1.30f);
			}
			PrefetchAll();
		}

		virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override
		{
			SCompoundWidget::OnMouseLeave(MouseEvent);
			bHovered = false;
			FrameIndex = 0;
			if (ScaleBox.IsValid())
			{
				ScaleBox->SetUserSpecifiedScale(1.0f);
			}
			RefreshImage();
		}

		virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event) override
		{
			if (Event.GetEffectingButton() == EKeys::LeftMouseButton && ClickHandler.IsBound())
			{
				ClickHandler.Execute();
				return FReply::Handled();
			}
			return FReply::Unhandled();
		}

	private:
		EActiveTimerReturnType Animate(double, float)
		{
			if (!bHovered)
			{
				TimerHandle.Reset();
				return EActiveTimerReturnType::Stop;
			}
			if (Frames > 0)
			{
				FrameIndex = (FrameIndex + 1) % Frames;
				RefreshImage();
			}
			return EActiveTimerReturnType::Continue;
		}

		void PrefetchAll()
		{
			if (!BrushProvider || Frames <= 0)
			{
				return;
			}
			for (int32 Index = 0; Index < Frames; ++Index)
			{
				BrushProvider(FrameUrl(Index), FrameSize); // kicks off the download
			}
		}

		FString FrameUrl(int32 Index) const
		{
			return FString::Printf(TEXT("%s%02d.png"), *UrlPrefix, Index);
		}

		void RefreshImage()
		{
			if (!ImageWidget.IsValid() || !BrushProvider)
			{
				return;
			}
			const FSlateBrush* BrushPtr = BrushProvider(FrameUrl(FrameIndex), FrameSize);
			if (BrushPtr)
			{
				ImageWidget->SetImage(BrushPtr);
			}
		}

		FString Name;
		FString ClassName;
		FString UrlPrefix;
		int32 Frames = 0;
		FVector2D FrameSize = FVector2D(208.f, 117.f);
		bool bSelected = false;
		bool bHovered = false;
		int32 FrameIndex = 0;
		FSimpleDelegate ClickHandler;
		TFunction<const FSlateBrush*(const FString&, FVector2D)> BrushProvider;
		TSharedPtr<SImage> ImageWidget;
		TSharedPtr<SScaleBox> ScaleBox;
		TSharedPtr<FActiveTimerHandle> TimerHandle;
	};
}

TSharedRef<SWidget> STestPlatformUI::VehicleCard(const FVehicleInfo& Vehicle, bool bSelected,
	TFunction<void()> OnClicked)
{
	TSharedRef<SVehicleCard> Card = SNew(SVehicleCard)
		.Name(Vehicle.Name)
		.ClassName(Vehicle.Cls)
		.UrlPrefix(FString::Printf(TEXT("/vehicle_spin/%s/"), *Vehicle.Id))
		.Frames(SpinFrames)
		.FrameWidth(208.f)
		.FrameHeight(117.f)
		.Selected(bSelected)
		.OnClicked(FSimpleDelegate::CreateLambda([OnClicked]()
		{
			if (OnClicked)
			{
				OnClicked();
			}
		}));
	Card->SetBrushProvider([this](const FString& Url, FVector2D Size)
	{
		return Thumb(Url, Size);
	});
	// plate stays card-sized; the art inside is larger so it can spill over on hover
	return SNew(SBorder)
		.BorderImage(&Brush(bSelected ? ColCardSel : ColCard))
		.Padding(FMargin(2.f))
		[
			Card
		];
}
// ---------------------------------------------------------------------- //
// right column
// ---------------------------------------------------------------------- //
TSharedRef<SWidget> STestPlatformUI::MakeRightColumn()
{
	return SNew(SBorder)
		.BorderImage(&Brush(ColPanel))
		.Padding(FMargin(10.f, 10.f))
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(FMargin(0.f, 0.f, 0.f, 8.f))
			[
				SAssignNew(AgentBox, SVerticalBox)
			]
			+ SScrollBox::Slot().Padding(FMargin(0.f, 0.f, 0.f, 8.f))
			[
				SAssignNew(RunBox, SVerticalBox)
			]
			+ SScrollBox::Slot()
			[
				SAssignNew(SummaryBox, SVerticalBox)
			]
		];
}

TSharedRef<SWidget> STestPlatformUI::Chip(const FString& Text, bool bSelected,
	TFunction<void()> OnClicked, const FString& Tooltip)
{
	TSharedRef<SButton> Button = SNew(SButton)
		.ButtonStyle(bSelected ? &BtnChipOn() : &BtnChipIdle())
		.ContentPadding(FMargin(10.f, 5.f))
		.OnClicked_Lambda([OnClicked]()
		{
			if (OnClicked)
			{
				OnClicked();
			}
			return FReply::Handled();
		})
		[
			::Text(Text, 10, bSelected, bSelected ? ColText : ColMuted)
		];
	if (!Tooltip.IsEmpty())
	{
		Button->SetToolTipText(FText::FromString(Tooltip));
	}
	return Button;
}

// ---------------------------------------------------------------------- //
// algorithm parameters, rendered from the backend's own schema
//
// Everything below dispatches on FAlgoParam::Type and reads the schema's own
// group / range / unit / default. No parameter key is named anywhere, which is
// the point: a new algorithm or a new parameter is a backend-only change.
// ---------------------------------------------------------------------- //
FAlgoParamValue STestPlatformUI::ResolvedParam(const FAlgoParam& Param) const
{
	return ResolveAlgoParamValue(Param, Config.AlgoParams);
}

void STestPlatformUI::SetParamNumber(const FAlgoParam& Param, float Value)
{
	FAlgoParamValue& Stored = Config.AlgoParams.FindOrAdd(Param.Key);
	Stored.Type = Param.Type;
	Stored.Number = FMath::Clamp(Value, Param.Min, Param.Max);
	Stored.bUserSet = true;
}

void STestPlatformUI::SetParamBool(const FAlgoParam& Param, bool bValue)
{
	FAlgoParamValue& Stored = Config.AlgoParams.FindOrAdd(Param.Key);
	Stored.Type = Param.Type;
	Stored.bBool = bValue;
	Stored.bUserSet = true;
}

void STestPlatformUI::SetParamString(const FAlgoParam& Param, const FString& Value)
{
	FAlgoParamValue& Stored = Config.AlgoParams.FindOrAdd(Param.Key);
	Stored.Type = Param.Type;
	Stored.StringValue = Value;
	Stored.bUserSet = true;
}

void STestPlatformUI::SyncAlgoParamsToSchema()
{
	const FAlgoInfo* Algo = Api.IsValid() ? Api->SelectedAlgorithm(Config.AlgoId) : nullptr;
	if (!Algo)
	{
		return;
	}
	TSet<FString> Declared;
	for (const FAlgoParam& Param : Algo->Params)
	{
		Declared.Add(Param.Key);
	}
	// Keep what the new schema still declares (same key, same type); drop the
	// rest. A key the algorithm does not take would otherwise travel in the
	// config and be silently ignored by the backend -- or worse, be accepted by
	// a different algorithm that happens to use the same name for something
	// else.
	for (auto It = Config.AlgoParams.CreateIterator(); It; ++It)
	{
		const FAlgoParam* Param = Algo->Params.FindByPredicate(
			[&Key = It.Key()](const FAlgoParam& Candidate) { return Candidate.Key == Key; });
		if (Param == nullptr || Param->Type != It.Value().Type)
		{
			UE_LOG(LogTestPlatformUI, Log,
				TEXT("algorithm %s: dropping stored parameter '%s' (%s) — not in its schema"),
				*Algo->Id, *It.Key(), *It.Value().Type);
			GLog->Flush();
			It.RemoveCurrent();
		}
	}
}

TSharedRef<SWidget> STestPlatformUI::AlgoParamRow(const FAlgoParam& Param)
{
	if (Param.Type == TEXT("enum"))
	{
		return EnumParamRow(Param);
	}
	if (Param.Type == TEXT("number"))
	{
		return NumberParamRow(Param);
	}
	if (Param.Type == TEXT("bool"))
	{
		return BoolParamRow(Param);
	}
	return UnknownParamRow(Param);
}

TSharedRef<SWidget> STestPlatformUI::EnumParamRow(const FAlgoParam& Param)
{
	TSharedRef<SWrapBox> Wrap = SNew(SWrapBox).UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(6.f, 6.f));
	if (Param.Options.Num() == 0)
	{
		// An enum with no options cannot be chosen; say so instead of drawing an
		// empty row that looks broken.
		Wrap->AddSlot()[Text(TEXT("（schema 未给出 options）"), 9, false, ColDim)];
	}
	const FString Current = ResolvedParam(Param).StringValue;
	for (const FAlgoOption& Option : Param.Options)
	{
		const FString Value = Option.Value;
		Wrap->AddSlot()
		[
			Chip(Option.Label, Value == Current, [this, Param, Value]()
			{
				SetParamString(Param, Value);
				RebuildAgent();
				RebuildSummary();
			})
		];
	}
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 3.f))
		[Text(AlgoParamLabel(Param), 10, false, ColMuted)]
		+ SVerticalBox::Slot().AutoHeight()[Wrap];
}

TSharedRef<SWidget> STestPlatformUI::NumberParamRow(const FAlgoParam& Param)
{
	const FAlgoParamValue Current = ResolvedParam(Param);
	const float Step = Param.Step;
	const FString Unit = Param.Unit;

	// The value is drawn from ResolvedParam every frame, so a control the user
	// has not touched still shows the schema's default (and follows it if the
	// schema changes underneath).
	TSharedRef<STextBlock> ValueText = SNew(STextBlock)
		.Text_Lambda([this, Param]()
		{
			return FText::FromString(
				FormatParamNumber(ResolvedParam(Param).Number, Param.Step, Param.Unit));
		})
		.Font(F(10, true));

	TSharedRef<SVerticalBox> Row = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[Text(AlgoParamLabel(Param), 10, false, ColMuted)]
			+ SHorizontalBox::Slot().AutoWidth()[ValueText]
		];

	if (!Param.bHasRange)
	{
		// No min/max in the schema: there is no range to put on a slider, so the
		// row is a stepper over the declared step instead of an invented 0..100.
		Row->AddSlot().AutoHeight().Padding(FMargin(0.f, 2.f, 0.f, 0.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&BtnGhost()).ContentPadding(FMargin(10.f, 3.f))
				.OnClicked_Lambda([this, Param]()
				{
					SetParamNumber(Param, ResolvedParam(Param).Number - Param.Step);
					return FReply::Handled();
				})
				[Text(TEXT("−"), 11)]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4.f, 0.f))
			[
				SNew(SButton).ButtonStyle(&BtnGhost()).ContentPadding(FMargin(10.f, 3.f))
				.OnClicked_Lambda([this, Param]()
				{
					SetParamNumber(Param, ResolvedParam(Param).Number + Param.Step);
					return FReply::Handled();
				})
				[Text(TEXT("+"), 11)]
			]
		];
		return Row;
	}

	Row->AddSlot().AutoHeight().Padding(FMargin(0.f, 2.f, 0.f, 0.f))
	[
		SNew(SSlider)
		.MinValue(Param.Min)
		.MaxValue(Param.Max)
		.StepSize(Step)
		.Value_Lambda([this, Param]()
		{
			return ResolvedParam(Param).Number;
		})
		.OnValueChanged_Lambda([this, Param](float NewValue)
		{
			// Round to the schema's step so the value the backend receives is one
			// the schema allows.
			const float Snapped = Param.Step > 0.f
				? FMath::RoundToFloat(NewValue / Param.Step) * Param.Step
				: NewValue;
			SetParamNumber(Param, Snapped);
		})
	];
	return Row;
}

TSharedRef<SWidget> STestPlatformUI::BoolParamRow(const FAlgoParam& Param)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, Param]()
			{
				return ResolvedParam(Param).bBool
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, Param](ECheckBoxState NewState)
			{
				SetParamBool(Param, NewState == ECheckBoxState::Checked);
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 0.f, 0.f))
		[Text(AlgoParamLabel(Param), 10, false, ColMuted)];
}

TSharedRef<SWidget> STestPlatformUI::UnknownParamRow(const FAlgoParam& Param)
{
	// A type this build cannot render: show the key, its type and the schema's
	// default, read-only, and say so exactly once. Dropping it silently would
	// make a backend-only schema change look like it worked when it did not.
	if (!UnknownParamTypesLogged.Contains(Param.Type))
	{
		UnknownParamTypesLogged.Add(Param.Type);
		UE_LOG(LogTestPlatformUI, Warning,
			TEXT("algorithm parameter '%s' has unsupported type '%s': rendered read-only "
				"with its default (add a row type to AlgoParamRow to support it)"),
			*Param.Key, *Param.Type);
		GLog->Flush();
	}
	const FAlgoParamValue Value = ResolvedParam(Param);
	const FString Shown = Param.Type == TEXT("number")
		? FormatParamNumber(Value.Number, Param.Step, Param.Unit)
		: Value.StringValue;
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[Text(AlgoParamLabel(Param), 10, false, ColMuted)]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[Text(FString::Printf(TEXT("%s · %s"), *Param.Type, *Shown), 9, false, ColDim)];
}

TSharedRef<SWidget> STestPlatformUI::KV(const FString& Key, const FString& Value)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.42f)[Text(Key, 10, false, ColDim)]
		+ SHorizontalBox::Slot().FillWidth(0.58f)[Text(Value, 10, true, ColText)];
}

void STestPlatformUI::RebuildRouteChips()
{
	if (!RouteChipsBox.IsValid() || !Api.IsValid())
	{
		return;
	}
	RouteChipsBox->ClearChildren();
	TSharedRef<SWrapBox> Wrap = SNew(SWrapBox).UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(6.f, 6.f));
	for (const FMapPreset& Preset : Api->PresetsFor(Config.MapId))
	{
		const FString Id = Preset.Id;
		Wrap->AddSlot()
		[
			Chip(PresetLabel(Preset), Id == Config.RoutePresetId, [this, Id]()
			{
				Config.RoutePresetId = Id;
				if (Api.IsValid() && !SessionId.IsEmpty())
				{
					Api->SetRoute(SessionId, true, false, Id, nullptr);
				}
				RebuildRouteChips();
				RebuildSummary();
			})
		];
	}
	Wrap->AddSlot()
	[
		Chip(bShowRoute ? LOCTEXT("HideRoute", "隐藏路线").ToString()
			: LOCTEXT("ShowRoute", "显示路线").ToString(), bShowRoute,
			[this]()
			{
				bShowRoute = !bShowRoute;
				if (Api.IsValid() && !SessionId.IsEmpty())
				{
					Api->SetRoute(SessionId, bShowRoute, true, FString(), nullptr);
				}
				RebuildRouteChips();
			},
			LOCTEXT("ShowRouteTip", "在 3D 画面里叠加显示规划路线").ToString())
	];
	Wrap->AddSlot()
	[
		Chip(LOCTEXT("CustomRoute", "自定义 road / lane / s").ToString(), false, nullptr,
			LOCTEXT("CustomRouteTip", "自定义路线（下一步接入）").ToString())
	];
	RouteChipsBox->AddSlot().AutoHeight()[Wrap];
}

FString STestPlatformUI::PresetLabel(const FMapPreset& Preset) const
{
	if (Preset.LengthKm > 0.f)
	{
		return FString::Printf(TEXT("%s %.2f km"), *Preset.Name, Preset.LengthKm);
	}
	return Preset.Name;
}

void STestPlatformUI::RebuildVehicles()
{
	if (!VehicleGrid.IsValid() || !Api.IsValid())
	{
		return;
	}
	VehicleGrid->ClearChildren();
	int32 Shown = 0;
	for (const FVehicleInfo& Vehicle : Api->Vehicles())
	{
		if (!Vehicle.Id.StartsWith(TEXT("vehicle.")) || Vehicle.Cls.IsEmpty() || Shown >= 9)
		{
			continue;
		}
		++Shown;
		const FString Id = Vehicle.Id;
		VehicleGrid->AddSlot()
		[
			VehicleCard(Vehicle, Id == Config.EgoBlueprint, [this, Id]()
			{
				Config.EgoBlueprint = Id;
				RebuildVehicles();
				RebuildSummary();
			})
		];
	}
}

void STestPlatformUI::RebuildWeather()
{
	if (!WeatherGrid.IsValid() || !Api.IsValid())
	{
		return;
	}
	WeatherGrid->ClearChildren();
	for (const FWeatherInfo& Weather : Api->Weather())
	{
		const FString Id = Weather.Id;
		WeatherGrid->AddSlot()
		[
			Chip(Weather.Name, Id == Config.WeatherId, [this, Id]()
			{
				Config.WeatherId = Id;
				if (Api.IsValid() && !SessionId.IsEmpty())
				{
					Api->SetWeather(SessionId, Id, nullptr); // live: preview repaints
				}
				RebuildWeather();
				RebuildSummary();
			}, Weather.Desc)
		];
	}
}

void STestPlatformUI::RebuildAgent()
{
	if (!AgentBox.IsValid() || !Api.IsValid())
	{
		return;
	}
	AgentBox->ClearChildren();

	AgentBox->AddSlot().AutoHeight()
	[
		NumberedTitle(TEXT("5"), LOCTEXT("AgentTitle", "智驾算法").ToString(), TEXT("carla.agents"))
	];

	TSharedRef<SWrapBox> AlgWrap = SNew(SWrapBox).UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(6.f, 6.f));
	for (const FAlgoInfo& Algo : Api->Algorithms())
	{
		const FString Id = Algo.Id;
		AlgWrap->AddSlot()
		[
			Chip(Algo.Name, Id == Config.AlgoId, [this, Id]()
			{
				Config.AlgoId = Id;
				// Switching algorithm: values whose key the new schema still
				// declares are kept, everything else is dropped, and every
				// parameter the user has not touched shows that schema's own
				// default again. Keeping a key the new algorithm does not declare
				// would post a parameter it never asked for; keeping a touched
				// value that *is* still declared (target_speed_kmh exists on both
				// built-ins) preserves the user's intent across the switch.
				SyncAlgoParamsToSchema();
				RebuildAgent();
				RebuildSummary();
			}, Algo.Desc)
		];
	}
	AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))[AlgWrap];

	AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
	[
		WrappedText(LOCTEXT("AlgoNote",
			"参数由算法自描述：/algorithms 返回每个参数的 schema（类型 / 范围 / 单位 / 默认值 / 分组），"
			"面板按 schema 渲染控件，新增算法或参数无需改面板。").ToString(),
			RightColumnContentWidth, 9, false, ColDim)
	];

	SyncAlgoParamsToSchema();

	const FAlgoInfo* Algo = Api->SelectedAlgorithm(Config.AlgoId);
	if (!Algo)
	{
		AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
		[Text(LOCTEXT("NoAlgo", "目录里没有这个算法").ToString(), 10, false, ColWarn)];
		return;
	}
	if (Algo->Params.Num() == 0)
	{
		AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
		[Text(LOCTEXT("NoParams", "该算法没有可调参数").ToString(), 10, false, ColDim)];
		return;
	}

	// Grouping and ordering come from the schema: params are emitted in
	// declaration order, and a group heading appears the first time a group is
	// met. A parameter with no group is emitted where it is declared, without a
	// heading, so a schema that declares no groups renders as a flat list.
	FString CurrentGroup;
	bool bFirstGroup = true;
	for (const FAlgoParam& Param : Algo->Params)
	{
		if (Param.Group != CurrentGroup)
		{
			CurrentGroup = Param.Group;
			if (!CurrentGroup.IsEmpty())
			{
				AgentBox->AddSlot().AutoHeight()
					.Padding(FMargin(0.f, bFirstGroup ? 8.f : 12.f, 0.f, 2.f))
				[
					Text(CurrentGroup, 9, true, ColDim)
				];
			}
			else if (bFirstGroup)
			{
				// no heading, but keep the block off the hint paragraph
				AgentBox->AddSlot().AutoHeight()[Spacer(1.f)];
			}
		}
		AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 5.f, 0.f, 0.f))
		[AlgoParamRow(Param)];
		bFirstGroup = false;
	}
}

void STestPlatformUI::RebuildRunSettings()
{
	if (!RunBox.IsValid() || !Api.IsValid())
	{
		return;
	}
	RunBox->ClearChildren();

	RunBox->AddSlot().AutoHeight()
	[
		NumberedTitle(TEXT("6"), LOCTEXT("RunTitle", "运行设置").ToString(),
			LOCTEXT("RunAside", "world settings").ToString())
	];

	RunBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)[Text(LOCTEXT("Timestep", "时间步长").ToString(), 10, false, ColMuted)]
		+ SHorizontalBox::Slot().AutoWidth()[Text(TEXT("0.05 s · 20 Hz"), 10, true)]
	];

	// background traffic
	TSharedRef<SWrapBox> TrafficWrap = SNew(SWrapBox).UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(6.f, 6.f));
	for (const FTrafficInfo& Traffic : Api->Traffic())
	{
		const FString Id = Traffic.Id;
		const FString Label = Traffic.Id == TEXT("none")
			? Traffic.Name
			: FString::Printf(TEXT("%s %d"), *Traffic.Name, Traffic.Vehicles);
		TrafficWrap->AddSlot()
		[
			Chip(Label, Id == Config.TrafficId, [this, Id]()
			{
				Config.TrafficId = Id;
				RebuildRunSettings();
				RebuildSummary();
			})
		];
	}
	RunBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 8.f, 0.f))
		[Text(LOCTEXT("Traffic", "背景车").ToString(), 10, false, ColMuted)]
		+ SHorizontalBox::Slot().FillWidth(1.f)[TrafficWrap]
	];

	TSharedRef<SWrapBox> RecordWrap = SNew(SWrapBox).UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(6.f, 6.f));
	RecordWrap->AddSlot()[Chip(TEXT("recorder"), true, nullptr)];
	RecordWrap->AddSlot()[Chip(TEXT("关闭"), false, nullptr)];
	RunBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 8.f, 0.f))
		[Text(LOCTEXT("Record", "录制").ToString(), 10, false, ColMuted)]
		+ SHorizontalBox::Slot().FillWidth(1.f)[RecordWrap]
	];

	TSharedRef<SWrapBox> ExportWrap = SNew(SWrapBox).UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(6.f, 6.f));
	ExportWrap->AddSlot()[Chip(TEXT("metrics.csv"), true, nullptr)];
	ExportWrap->AddSlot()[Chip(TEXT("events.json"), true, nullptr)];
	ExportWrap->AddSlot()[Chip(TEXT("config.json"), false, nullptr)];
	RunBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 8.f, 0.f))
		[Text(LOCTEXT("Export", "导出").ToString(), 10, false, ColMuted)]
		+ SHorizontalBox::Slot().FillWidth(1.f)[ExportWrap]
	];
}

void STestPlatformUI::RebuildSummary()
{
	if (!SummaryBox.IsValid() || !Api.IsValid())
	{
		return;
	}
	SummaryBox->ClearChildren();

	const FMapInfo* Map = Api->FindMap(Config.MapId);
	const FVehicleInfo* Vehicle = Api->Vehicles().FindByPredicate(
		[this](const FVehicleInfo& Info) { return Info.Id == Config.EgoBlueprint; });
	const FWeatherInfo* Weather = Api->Weather().FindByPredicate(
		[this](const FWeatherInfo& Info) { return Info.Id == Config.WeatherId; });
	const FMapPreset* Preset = Api->PresetsFor(Config.MapId).FindByPredicate(
		[this](const FMapPreset& Info) { return Info.Id == Config.RoutePresetId; });

	SummaryBox->AddSlot()
	[
		Card(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				NumberedTitle(TEXT("="), LOCTEXT("Summary", "本次配置").ToString(),
					LOCTEXT("SummaryAside", "待确认").ToString())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
			[KV(LOCTEXT("SumMap", "地图").ToString(), Map ? Map->Id : TEXT("—"))]
			+ SVerticalBox::Slot().AutoHeight()
			[KV(LOCTEXT("SumEgo", "主车").ToString(), Vehicle ? Vehicle->Name : TEXT("—"))]
			+ SVerticalBox::Slot().AutoHeight()
			[KV(LOCTEXT("SumRoute", "路线").ToString(), Preset ? PresetLabel(*Preset) : TEXT("—"))]
			+ SVerticalBox::Slot().AutoHeight()
			[KV(LOCTEXT("SumWeather", "天气").ToString(), Weather ? Weather->Name : TEXT("—"))]
			+ SVerticalBox::Slot().AutoHeight()
			[KV(LOCTEXT("SumAgent", "算法").ToString(), Config.AlgoId)]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
			[
				WrappedText(LOCTEXT("SumHint",
					"路线按 xodr 生成（road_id / lane_id / s），坐标用到时才解析：首尾点解析失败会报错，中间点解析不到会跳过。").ToString(),
					RightCardContentWidth, 9, false, ColWarn)
			])
	];
}

// ---------------------------------------------------------------------- //
// bottom bar
// ---------------------------------------------------------------------- //
TSharedRef<SWidget> STestPlatformUI::MakeBottomBar()
{
	return SNew(SBorder)
		.BorderImage(&Brush(ColShell))
		.Padding(FMargin(14.f, 8.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SAssignNew(StatusText, STextBlock).Font(F(10))
				.ColorAndOpacity(FSlateColor(ColMuted))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.f, 0.f, 8.f, 0.f))
			[
				SNew(SButton).ButtonStyle(&BtnGhost()).ContentPadding(FMargin(14.f, 7.f))
				.Visibility_Lambda([this]()
				{
					return (Session.IsActive() || Session.IsRunning())
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				.OnClicked_Lambda([this]() { StopSessionNow(); return FReply::Handled(); })
				[Text(LOCTEXT("Stop", "停止").ToString(), 11)]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton).ButtonStyle(&BtnPrimary()).ContentPadding(FMargin(26.f, 9.f))
				.OnClicked_Lambda([this]() { OnPrimaryClicked(); return FReply::Handled(); })
				[
					SAssignNew(PrimaryBottomLabel, STextBlock)
					.Text(LOCTEXT("Enter2", "进入 CARLA 世界 →"))
					.Font(F(12, true)).ColorAndOpacity(FSlateColor(ColInk))
				]
			]
		];
}

TSharedRef<SWidget> STestPlatformUI::MakeLoadingOverlay()
{
	return SNew(SBorder)
		.BorderImage(&Brush(FLinearColor(0.030f, 0.040f, 0.052f, 0.97f)))
		.Padding(FMargin(28.f, 22.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SAssignNew(LoadingText, STextBlock).Text(FText::FromString(TEXT("正在加载地图…")))
				.Font(F(14, true)).ColorAndOpacity(FSlateColor(ColText))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 8.f, 0.f, 0.f))
			[
				Text(LOCTEXT("LoadingHint",
					"首次加载一张地图约 20–40 s，之后切换同一张地图会复用已加载的世界。").ToString(),
					10, false, ColMuted)
			]
		];
}

// ---------------------------------------------------------------------- //
// actions
// ---------------------------------------------------------------------- //
void STestPlatformUI::OnSelectMap(const FString& MapId)
{
	Config.MapId = MapId;
	const TArray<FMapPreset>& Presets = Api.IsValid() ? Api->PresetsFor(MapId) : TArray<FMapPreset>();
	if (Presets.Num() > 0)
	{
		const bool bStillValid = Presets.ContainsByPredicate(
			[this](const FMapPreset& P) { return P.Id == Config.RoutePresetId; });
		if (!bStillValid)
		{
			Config.RoutePresetId = Presets[0].Id;
		}
	}
	RebuildMapRows();
	RebuildRouteChips();
	RebuildSummary();
	if (Api.IsValid())
	{
		Api->SelectMap(MapId);
	}
	// heavy maps block the game thread for a while: warn first, then load
	if (MapId == TEXT("Town10HD") || MapId == TEXT("Town12") || MapId == TEXT("Town13"))
	{
		SetStatus(FString::Printf(
			TEXT("%s 是高清地图，加载期间引擎会阻塞 30–90 s（界面暂不响应属正常），加载完会立刻恢复"),
			*MapId), true);
	}

	// live switch: load the map now, so the viewport follows the selection
	LoadSelectedMap([this](bool bOk)
	{
		if (bOk)
		{
			SetStatus(FString::Printf(TEXT("已加载 %s，点“进入 CARLA 世界”开始运行"), *Config.MapId));
		}
	});
}

void STestPlatformUI::LoadSelectedMap(TFunction<void(bool)> Done)
{
	if (!Api.IsValid() || Config.MapId.IsEmpty())
	{
		Done(false);
		return;
	}
	if (bLoadingMap)
	{
		// remember the latest request; it starts as soon as the current load ends
		QueuedMapId = Config.MapId;
		Done(false);
		return;
	}
	bLoadingMap = true;
	LoadStartedAt = FPlatformTime::Seconds();
	bBusy = true;
	SyncCarlaProbeWindow();
	if (LoadingBox.IsValid())
	{
		LoadingBox->SetVisibility(EVisibility::Visible);
	}
	if (LoadingText.IsValid())
	{
		LoadingText->SetText(FText::FromString(
			FString::Printf(TEXT("正在加载地图 %s …"), *Config.MapId)));
	}
	SetStatus(FString::Printf(TEXT("正在加载地图 %s …"), *Config.MapId));

	// The module's map gate records which load was requested by this panel:
	// that is the only world change an entry run keeps its panel through.
	if (OnPreviewRequest)
	{
		OnPreviewRequest(Config.MapId);
	}
	Api->PreviewMap(Config, [this, Done](bool bOk, const FSessionStatus& Status, const FString& Error)
	{
		bLoadingMap = false;
		bBusy = false;
		SyncCarlaProbeWindow();
		if (LoadingBox.IsValid())
		{
			LoadingBox->SetVisibility(EVisibility::Collapsed);
		}
		if (!QueuedMapId.IsEmpty())
		{
			const FString Next = QueuedMapId;
			QueuedMapId.Reset();
			if (Next != this->Config.MapId)
			{
				this->Config.MapId = Next;
				this->RebuildMapRows();
			}
			this->LoadSelectedMap(nullptr);
			if (Done)
			{
				Done(false);
			}
			return;
		}
		if (bOk)
		{
			Session = Status;
			SessionId = Status.Id;
			SessionMapId = Config.MapId;
			SetStatus(FString::Printf(TEXT("已加载 %s，点“进入 CARLA 世界”开始运行"), *Config.MapId));
			RebuildSummary();
		}
		else
		{
			// The load never started (bad map id, backend gone): no world change
			// for this map is coming, so the module drops it as the expected
			// preview and only a live, requested one can keep the panel armed.
			if (OnPreviewFailed)
			{
				OnPreviewFailed();
			}
			SetStatus(FString::Printf(TEXT("加载地图失败：%s"), *Error), true);
		}
		if (Done)
		{
			Done(bOk);
		}
	});
}

void STestPlatformUI::OnPrimaryClicked()
{
	if (bBusy || bLoadingMap)
	{
		return;
	}
	if (Session.IsRunning())
	{
		StopSessionNow();
		return;
	}
	// entering the world: make sure the selected map is the one that is loaded
	if (SessionId.IsEmpty() || SessionMapId != Config.MapId || !Session.IsPreviewing())
	{
		LoadSelectedMap([this](bool bOk)
		{
			if (bOk)
			{
				StartSessionNow();
			}
		});
		return;
	}
	StartSessionNow();
}

void STestPlatformUI::BeginLoadAndStart()
{
	LoadSelectedMap([this](bool bOk)
	{
		if (bOk)
		{
			StartSessionNow();
		}
	});
}

void STestPlatformUI::StartSessionNow()
{
	if (!Api.IsValid() || SessionId.IsEmpty())
	{
		SetStatus(LOCTEXT("NoSession", "还没有可用的会话，请先选地图").ToString(), true);
		return;
	}
	bBusy = true;
	SetStatus(LOCTEXT("Starting", "启动主车…").ToString());
	// The session config was posted when the map was previewed, and /start builds
	// the agent from *that* copy: parameter edits made after the preview would be
	// invisible to it. Push the current parameter block first, so what starts is
	// what the panel shows. A failed push is reported, never silently skipped --
	// starting with stale parameters and calling it success is the bug this
	// whole change exists to remove.
	Api->SetAlgorithm(SessionId, Config,
		[this](bool bOk, const FString& Error)
	{
		if (!bOk)
		{
			bBusy = false;
			SetStatus(FString::Printf(TEXT("启动失败：参数未能下发（%s）"), *Error), true);
			return;
		}
		Api->StartSession(SessionId,
			[this](bool bStarted, const FSessionStatus& Status, const FString& StartError)
		{
			bBusy = false;
			if (bStarted)
			{
				Session = Status;
				SetUIVisible(false); // the world takes the screen
				SetStatus(LOCTEXT("Running3", "运行中").ToString());
			}
			else
			{
				SetStatus(FString::Printf(TEXT("启动失败：%s"), *StartError), true);
			}
		});
	});
}

void STestPlatformUI::StopSessionNow()
{
	if (!Api.IsValid() || SessionId.IsEmpty())
	{
		return;
	}
	bBusy = true;
	SetStatus(LOCTEXT("Stopping", "停止中…").ToString());
	Api->StopSession(SessionId, [this](bool bOk, const FSessionStatus& Status, const FString& Error)
	{
		bBusy = false;
		Session = Status;
		SetUIVisible(true);
		SetStatus(bOk
			? LOCTEXT("Stopped", "已停止，结果已导出到 runs/<session_id>/").ToString()
			: FString::Printf(TEXT("停止失败：%s"), *Error), !bOk);
	});
}



void STestPlatformUI::SetStatus(const FString& InText, bool bWarn)
{
	StatusDetail = InText;
	bWarnStatus = bWarn;
	UpdateChrome();
}

void STestPlatformUI::SetUIVisible(bool bVisible)
{
	bUIVisible = bVisible;
}

void STestPlatformUI::SetSessionStatus(const FSessionStatus& Status)
{
	// Whether a load is in flight is a fact the top bar needs even when the rest
	// of the session status is suppressed below: /health cannot answer honestly
	// during it (see UpdateChrome). /sessions/current is where the backend says
	// it, so read it before the early return.
	SetSessionLoadInFlight(Status.bValid && Status.State == TEXT("preview_loading"));
	if (bBusy || bLoadingMap)
	{
		return;
	}
	Session = Status;
	if (Status.bValid && !Status.Id.IsEmpty())
	{
		SessionId = Status.Id;
	}
	if (Status.IsRunning())
	{
		SetStatus(FString::Printf(TEXT("运行中 · %.1f km/h · %.1f s · %d 事件"),
			Status.SpeedKmh, Status.SimTime, Status.Events));
	}
	UpdateChrome();
}

void STestPlatformUI::SetSessionLoadInFlight(bool bInFlight)
{
	const bool bChanged = bSessionLoadInFlight != bInFlight;
	bSessionLoadInFlight = bInFlight;
	if (bChanged)
	{
		SyncCarlaProbeWindow();
	}
}

void STestPlatformUI::SyncCarlaProbeWindow()
{
	const bool bBlocked = IsCarlaProbeBlocked();
	if (bBlocked == bProbeBlocked)
	{
		return;
	}
	bProbeBlocked = bBlocked;
	if (!Api.IsValid())
	{
		return;
	}
	if (bBlocked)
	{
		// A probe issued now would only measure the blocked simulator, and an
		// answer already in flight was measured in the same window: drop both.
		Api->InvalidateCarlaState();
	}
	else
	{
		// The load is over: ask now rather than waiting up to 5 s for the next
		// poll, and force=true so the backend does not hand back the verdict it
		// cached while the world was loading.
		LastHealthPollAt = FPlatformTime::Seconds();
		Api->LoadHealth(/*bForce=*/true);
	}
	UpdateChrome();
}

void STestPlatformUI::ReloadCatalog()
{
	LastCatalogAttemptAt = FPlatformTime::Seconds();
	if (Api.IsValid())
	{
		// No "rebuild on the next tick" flag here: the next tick would rebuild
		// from the cache that has not been replaced yet. TickRefresh rebuilds
		// when CatalogRevision() moves, which is when the answer has landed.
		Api->LoadCatalog();
	}
}

bool STestPlatformUI::IsBackendReady() const
{
	return Api.IsValid() && Api->IsReady();
}

bool STestPlatformUI::IsBackendReachable() const
{
	// HasBackendAnswer() keeps the card from claiming "not running" before the
	// first /health has come back: until then the state is simply unknown.
	return Api.IsValid() && Api->HasBackendAnswer() && Api->IsBackendResponding();
}

EVisibility STestPlatformUI::OfflineCardVisibility() const
{
	// Hidden only when the backend is answering *and* its catalog is loaded.
	// Because liveness is a live /health fact rather than "the catalog loaded
	// once", closing the backend window brings the card back over the cached
	// lists, which is what the operator needs to see.
	const bool bUsable = IsBackendReachable() && Api.IsValid() && Api->IsReady();
	return (bUIVisible && !bUsable) ? EVisibility::Visible : EVisibility::Collapsed;
}

FText STestPlatformUI::OfflineTitle() const
{
	if (!Api.IsValid())
	{
		return LOCTEXT("OfflineNoApi", "配置后端未运行");
	}
	// Three distinct states, so the card never says "not running" while the
	// backend answers: probing (no answer yet), adopting (answer, no catalog
	// yet), unreachable (the last probe got no answer).
	if (!Api->HasBackendAnswer())
	{
		return FText::FromString(FString::Printf(
			TEXT("正在检测后端（%s）…"), *FTestPlatformApi::RootUrl()));
	}
	if (Api->IsBackendResponding())
	{
		return LOCTEXT("OfflineAdopting", "后端已响应，正在接管…");
	}
	return FText::FromString(FString::Printf(
		TEXT("配置后端未运行（%s）"), *FTestPlatformApi::RootUrl()));
}

FText STestPlatformUI::OfflineHint() const
{
	if (!Api.IsValid() || !Api->HasBackendAnswer())
	{
		return LOCTEXT("OfflineProbingHint", "正在等待后端应答…");
	}
	if (!Api->IsBackendResponding())
	{
		return LOCTEXT("OfflineHint",
			"地图、路线与仿真会话都由它提供；启动后会自动接管，无需手动刷新。\n"
			"点「启动后端」在终端窗口中启动它，也可以手动运行 python run.py。");
	}
	if (!Api->IsReady())
	{
		return LOCTEXT("OfflineAdoptingHint",
			"后端已经应答，正在读取地图、车辆与算法列表；读完后这张卡片会自动消失。");
	}
	return FText::GetEmpty();
}

EVisibility STestPlatformUI::OfflineErrorVisibility() const
{
	return (!IsBackendReachable() && !BackendStartError.IsEmpty())
		? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility STestPlatformUI::StartBackendButtonVisibility() const
{
	// Only when the backend is unreachable; while a request is in flight the
	// disabled busy button stays visible until the outcome is known.
	if (bBackendStartPending)
	{
		return EVisibility::Visible;
	}
	const bool bUnreachable = Api.IsValid() && Api->HasBackendAnswer()
		&& !Api->IsBackendResponding();
	return bUnreachable ? EVisibility::Visible : EVisibility::Collapsed;
}

bool STestPlatformUI::IsStartBackendEnabled() const
{
	return !bBackendStartPending;
}

FText STestPlatformUI::StartBackendLabel() const
{
	return bBackendStartPending
		? LOCTEXT("BackendStartingBusy", "启动中…")
		: LOCTEXT("BackendStart", "启动后端");
}

void STestPlatformUI::RequestBackendStart()
{
	if (bBackendStartPending)
	{
		return;
	}
	// The button is only painted while unreachable, but /health may have
	// answered between that paint and the click: never start a second instance.
	if (IsBackendReachable())
	{
		SetStatus(LOCTEXT("BackendAlreadyUp", "后端已在应答，无需重复启动").ToString());
		return;
	}
	FString Error;
	if (!OnStartBackend || !OnStartBackend(Error))
	{
		BackendStartError = Error.IsEmpty()
			? LOCTEXT("BackendStartFailed", "启动后端失败").ToString()
			: Error;
		SetStatus(BackendStartError, true);
		return;
	}
	bBackendStartPending = true;
	BackendStartError.Reset();
	BackendStartRequestedAt = FPlatformTime::Seconds();
	SetStatus(LOCTEXT("BackendStarting", "正在终端窗口中启动后端…").ToString());
}

void STestPlatformUI::UpdateChrome()
{
	// Is the backend process answering right now? Everything the top bar says
	// about the session and its clock is a fact *it* reported, so when it stops
	// answering those chips must go neutral instead of repeating the last answer
	// forever (a dead backend used to leave "会话 · 预览中 / 同步模式 · 20 Hz" on
	// screen while the server chip already said 未知).
	const bool bBackendUp = Api.IsValid() && Api->HasBackendAnswer() && Api->IsBackendResponding();
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(StatusDetail));
		StatusText->SetColorAndOpacity(FSlateColor(bWarnStatus ? ColWarn : ColMuted));
	}
	if (SessionChipText.IsValid())
	{
		// 未知 is the only honest word while nothing answers: the last state the
		// backend reported is no longer current, and claiming it is exactly the
		// stale chip the review flagged.
		SessionChipText->SetText(FText::FromString(bBackendUp
			? FString::Printf(TEXT("会话 · %s"), *StateText(Session.State))
			: FString(TEXT("会话 · 未知"))));
		SessionChipText->SetColorAndOpacity(FSlateColor(bBackendUp ? StateColor() : ColDim));
	}
	if (SyncChipText.IsValid())
	{
		// The chip follows the backend's report of the clock it is really
		// running: 20 Hz (or whatever fixed delta) while synchronous, 已失去 once
		// the simulator dropped it, — once there is nothing true to print.
		FString SyncText = TEXT("同步模式 · —");
		FLinearColor SyncColor = ColDim;
		FString SyncTip = TEXT("后端没有报告同步状态");
		if (!bBackendUp)
		{
			SyncTip = TEXT("后端进程没有应答，同步状态未知");
		}
		else if (Session.bSyncLost)
		{
			SyncText = TEXT("同步模式 · 已失去");
			SyncColor = ColWarn;
			SyncTip = Session.SyncLostDetail.IsEmpty()
				? TEXT("模拟器已退出同步模式，仿真仍在运行（按最后已知步长计时）")
				: Session.SyncLostDetail;
		}
		else if (Session.IsActive() && Session.bSyncMode && Session.FixedDelta > 0.f)
		{
			const int32 Hz = FMath::Max(1, FMath::RoundToInt(1.f / Session.FixedDelta));
			SyncText = FString::Printf(TEXT("同步模式 · %d Hz"), Hz);
			SyncColor = ColMuted;
			SyncTip = FString::Printf(TEXT("固定步长 %.4f s（由后端报告）"), Session.FixedDelta);
		}
		SyncChipText->SetText(FText::FromString(SyncText));
		SyncChipText->SetColorAndOpacity(FSlateColor(SyncColor));
		SyncChipText->SetToolTipText(FText::FromString(SyncTip));
	}
	if (ServerChipText.IsValid())
	{
		// The address in this chip is CARLA's RPC endpoint, so it has to be the
		// endpoint the backend is really probing — GET /health reports it under
		// carla.host / carla.port (CARLA_HOST / CARLA_PORT in the backend). A
		// constant here prints a lie as soon as the backend is pointed elsewhere.
		// Until an answer carrying it has been parsed there is nothing honest to
		// print, so the address is a dash rather than an invented default.
		const FString Address = (Api.IsValid() && Api->HasCarlaEndpoint())
			? FString::Printf(TEXT("%s : %d"), *Api->CarlaHost(), Api->CarlaPort())
			: FString(TEXT("—"));
		// The state after the dot comes from the same probe (/health ->
		// carla.connected). "the backend answered" is not the same thing: the
		// backend serves the catalog with the simulator closed and still reports
		// connected=false.
		//
		// A failed probe is only evidence of "not connected" when CARLA had a
		// chance to answer. During a blocking load_world (30-90 s on the HD
		// maps) the probe's 1 s timeout always expires, so that window is its
		// own case: "could not tell" (未知), never 未连接. A probe that *succeeds*
		// during it is still displayed as 已连接 — that answer is positive
		// evidence, so there is no reason to hide it.
		const bool bBlocked = IsCarlaProbeBlocked();
		const bool bKnown = Api.IsValid() && Api->HasCarlaState();
		const bool bConnected = bKnown && Api->IsCarlaConnected();
		const FString State = bConnected
			? FString(TEXT("已连接"))
			: ((bKnown && !bBlocked) ? FString(TEXT("未连接")) : FString(TEXT("未知")));
		ServerChipText->SetText(FText::FromString(
			FString::Printf(TEXT("服务器 %s · %s"), *Address, *State)));
		// The dot reads the same State string, so it cannot drift from the text:
		// teal only when the probe says connected, grey (ColMuted) when it says
		// not connected, dimmer grey (ColDim) while nothing can be decided.
		if (ServerDot.IsValid())
		{
			const FLinearColor DotColor = (State == TEXT("已连接")) ? ColAccent
				: ((State == TEXT("未连接")) ? ColMuted : ColDim);
			ServerDot->SetBorderImage(&Brush(DotColor));
		}
	}
	if (MapChipText.IsValid())
	{
		MapChipText->SetText(FText::FromString(
			Config.MapId.IsEmpty() ? TEXT("未选地图") : Config.MapId));
	}
	if (WeatherChipText.IsValid())
	{
		const FWeatherInfo* Found = Api.IsValid()
			? Api->Weather().FindByPredicate(
				[this](const FWeatherInfo& Info) { return Info.Id == Config.WeatherId; })
			: nullptr;
		WeatherChipText->SetText(FText::FromString(Found ? Found->Name : TEXT("天气")));
	}
	if (CaptionText.IsValid())
	{
		const FMapInfo* Map = Api.IsValid() ? Api->FindMap(Config.MapId) : nullptr;
		CaptionText->SetText(FText::FromString(Map
			? FString::Printf(TEXT("%s · %d 条道路 · %d 路口 · road_id / lane_id / s"),
				*Map->Id, Map->Roads, Map->Junctions)
			: FString()));
	}
	if (PrimaryTopLabel.IsValid())
	{
		PrimaryTopLabel->SetText(bBusy || bLoadingMap
			? LOCTEXT("Working", "处理中…")
			: (Session.IsRunning() ? LOCTEXT("Running", "运行中 · 返回配置")
				: LOCTEXT("Enter", "进入 CARLA 世界 →")));
	}
	if (PrimaryBottomLabel.IsValid())
	{
		PrimaryBottomLabel->SetText(bBusy || bLoadingMap
			? LOCTEXT("Working2", "加载地图中…")
			: (Session.IsRunning() ? LOCTEXT("Running2", "运行中 · 返回配置")
				: LOCTEXT("Enter2", "进入 CARLA 世界 →")));
	}
}

FString STestPlatformUI::StateText(const FString& State) const
{
	if (State == TEXT("running")) return TEXT("运行中");
	if (State == TEXT("preview")) return TEXT("预览中");
	if (State == TEXT("preview_loading")) return TEXT("加载中");
	if (State == TEXT("stopping")) return TEXT("停止中");
	if (State == TEXT("stopped")) return TEXT("已停止");
	if (State == TEXT("error")) return TEXT("错误");
	if (State == TEXT("created")) return TEXT("待加载");
	return TEXT("未开始");
}

FLinearColor STestPlatformUI::StateColor() const
{
	if (Session.State == TEXT("running")) return ColAccent;
	if (Session.State == TEXT("error")) return ColWarn;
	return ColMuted;
}

void STestPlatformUI::RebuildAll()
{
	if (!Api.IsValid() || !Api->IsReady())
	{
		return;
	}
	if (Config.MapId.IsEmpty() && Api->Maps().Num() > 0)
	{
		// default scenario map: Town10 (HD) when the catalog has it
		const FString Preferred = TEXT("Town10HD");
		const bool bHasPreferred = Api->Maps().ContainsByPredicate(
			[&Preferred](const FMapInfo& Info) { return Info.Id == Preferred; });
		Config.MapId = bHasPreferred ? Preferred : Api->Maps()[0].Id;
	}
	if (Config.EgoBlueprint.IsEmpty() && Api->Vehicles().Num() > 0)
	{
		const FVehicleInfo* Tesla = Api->Vehicles().FindByPredicate(
			[](const FVehicleInfo& Info) { return Info.Id == TEXT("vehicle.tesla.model3"); });
		Config.EgoBlueprint = Tesla ? Tesla->Id : Api->Vehicles()[0].Id;
	}
	if (Config.WeatherId.IsEmpty() && Api->Weather().Num() > 0)
	{
		Config.WeatherId = Api->Weather()[0].Id;
	}
	if (Config.TrafficId.IsEmpty() && Api->Traffic().Num() > 0)
	{
		const FTrafficInfo* Light = Api->Traffic().FindByPredicate(
			[](const FTrafficInfo& Info) { return Info.Id == TEXT("light"); });
		Config.TrafficId = Light ? Light->Id : Api->Traffic()[0].Id;
	}
	if (Config.AlgoId.IsEmpty() && Api->Algorithms().Num() > 0)
	{
		Config.AlgoId = Api->Algorithms()[0].Id;
	}
	const TArray<FMapPreset>& Presets = Api->PresetsFor(Config.MapId);
	if (Presets.Num() > 0 && !Presets.ContainsByPredicate(
			[this](const FMapPreset& P) { return P.Id == Config.RoutePresetId; }))
	{
		Config.RoutePresetId = Presets[0].Id;
	}

	RebuildMapRows();
	RebuildRouteChips();
	RebuildVehicles();
	RebuildWeather();
	RebuildAgent();
	RebuildRunSettings();
	RebuildSummary();

	// open on the overview: the default map is already loaded by the simulator,
	// so this is cheap and the panel starts with the whole map in view
	if (SessionId.IsEmpty())
	{
		LoadSelectedMap(nullptr);
	}
}

EActiveTimerReturnType STestPlatformUI::TickRefresh(double Time, float DeltaTime)
{
	if (!Api.IsValid())
	{
		return EActiveTimerReturnType::Continue;
	}
	const double Now = FPlatformTime::Seconds();
	const bool bReady = Api->IsReady();
	const bool bReachable = IsBackendReachable();
	// Rebuild when the catalog itself moved, not "once, the first time it was
	// ready": a /algorithms answer that declares new parameters (or a whole new
	// algorithm) has to re-render the panel without an editor restart, and that
	// is exactly what a revision bump means. It also covers the first load, since
	// the initial revision is 0 and the first answer makes it 1.
	if (bReady && Api->CatalogRevision() != LastCatalogRevision)
	{
		LastCatalogRevision = Api->CatalogRevision();
		RebuildAll();
	}
	// The backend may come up after Construct: the module spawns it at ~10 s, or
	// the operator starts it with the button / by hand. Adopt it without a
	// click: retry the catalog while /health answers, never overlap attempts,
	// and stop for good once IsReady() is true (no reload on every tick).
	if (!bReady && bReachable && !Api->IsCatalogLoadInFlight() &&
		Now - LastCatalogAttemptAt >= 3.0)
	{
		LastCatalogAttemptAt = Now;
		Api->LoadCatalog();
	}
	if (!bLoggedBackendAnswer && bReachable)
	{
		bLoggedBackendAnswer = true;
		UE_LOG(LogTestPlatformUI, Log,
			TEXT("backend health answered: reachable %.1f s after panel construction"),
			Now - ConstructedAt);
		GLog->Flush();
	}
	if (bReady && !bLastBackendReady)
	{
		UE_LOG(LogTestPlatformUI, Log,
			TEXT("backend adopted: catalog ready with %d maps, %.1f s after panel construction"),
			Api->Maps().Num(), Now - ConstructedAt);
		GLog->Flush();
	}
	// Explicit 启动后端: busy until /health answers. A later answer hands over to
	// the adoption path above; a timeout puts the reason on the card instead of
	// spinning forever.
	if (bBackendStartPending)
	{
		if (bReachable)
		{
			bBackendStartPending = false;
			BackendStartError.Reset();
			LastCatalogAttemptAt = 0.0; // adopt on the next tick
			SetStatus(LOCTEXT("BackendUp", "后端已启动，正在接管…").ToString());
		}
		else if (Now - BackendStartRequestedAt > 20.0)
		{
			bBackendStartPending = false;
			BackendStartError = LOCTEXT("BackendStartTimeout",
				"启动失败：后端在 20 秒内没有应答。请查看「CARLA TestPlatform Backend」窗口里的错误输出。").ToString();
			SetStatus(BackendStartError, true);
		}
	}
	// a blocking LoadMap can outlive its request (engine-side freeze): never
	// leave the UI stuck in "loading" forever
	if (bLoadingMap && LoadStartedAt > 0.0 && Now - LoadStartedAt > 300.0)
	{
		bLoadingMap = false;
		bBusy = false;
		LoadStartedAt = 0.0;
		if (LoadingBox.IsValid())
		{
			LoadingBox->SetVisibility(EVisibility::Collapsed);
		}
		SetStatus(TEXT("地图加载超时（多发生在 Town10HD 这类高清图），请重试或换一张"), true);
	}
	// CARLA's connection is live state, not a launch-time fact: /health
	// re-probes it (the backend caches the answer for 5 s), so keep polling
	// while the panel is up instead of freezing whatever was true at startup.
	// Not while a load is in flight though: the probe would only measure the
	// block (see UpdateChrome / SyncCarlaProbeWindow). Poll faster while the
	// backend is not ready yet: that is the adoption heartbeat.
	SyncCarlaProbeWindow();
	const double HealthInterval = (bReady && bReachable) ? 5.0 : 2.0;
	if (!IsCarlaProbeBlocked() && Now - LastHealthPollAt >= HealthInterval)
	{
		LastHealthPollAt = Now;
		Api->LoadHealth();
	}
	const bool bEndpointKnown = Api->HasCarlaEndpoint();
	const int32 Port = bEndpointKnown ? Api->CarlaPort() : 0;
	if (bReady != bLastBackendReady ||
		bReachable != bLastBackendReachable ||
		Api->IsCarlaConnected() != bLastCarlaConnected ||
		Api->HasCarlaState() != bLastCarlaKnown ||
		bEndpointKnown != bLastCarlaEndpointKnown ||
		Port != LastCarlaPort)
	{
		bLastBackendReady = bReady;
		bLastBackendReachable = bReachable;
		bLastCarlaConnected = Api->IsCarlaConnected();
		bLastCarlaKnown = Api->HasCarlaState();
		bLastCarlaEndpointKnown = bEndpointKnown;
		LastCarlaPort = Port;
		UpdateChrome();
	}
	return EActiveTimerReturnType::Continue;
}

// ---------------------------------------------------------------------- //
// A transparent area over the world: drag pans, wheel zooms, double click resets.
class SViewportCatcher : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SViewportCatcher) {}
		SLATE_EVENT(FPointerEventHandler, OnDrag)
		SLATE_EVENT(FPointerEventHandler, OnWheel)
		SLATE_EVENT(FPointerEventHandler, OnDoubleClick)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		DragHandler = InArgs._OnDrag;
		WheelHandler = InArgs._OnWheel;
		DoubleClickHandler = InArgs._OnDoubleClick;
		SetCursor(EMouseCursor::GrabHandClosed);
		SetToolTipText(FText::FromString(TEXT("左键拖动平移 · 滚轮缩放 · 双击复位")));
		ChildSlot[SNew(SBox)];
	}

	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			bDragging = true;
			return FReply::Handled().CaptureMouse(SharedThis(this));
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		if (bDragging && DragHandler.IsBound())
		{
			return DragHandler.Execute(Geometry, Event);
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		if (Event.GetEffectingButton() == EKeys::LeftMouseButton && bDragging)
		{
			bDragging = false;
			return FReply::Handled().ReleaseMouseCapture();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		if (WheelHandler.IsBound())
		{
			return WheelHandler.Execute(Geometry, Event);
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		if (DoubleClickHandler.IsBound())
		{
			return DoubleClickHandler.Execute(Geometry, Event);
		}
		return FReply::Unhandled();
	}

private:
	FPointerEventHandler DragHandler;
	FPointerEventHandler WheelHandler;
	FPointerEventHandler DoubleClickHandler;
	bool bDragging = false;
};

TSharedRef<SWidget> STestPlatformUI::MakeViewportCatcher()
{
	return SNew(SViewportCatcher)
		.OnDrag(FPointerEventHandler::CreateLambda(
			[this](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (OnPan)
				{
					OnPan(Event.GetCursorDelta());
				}
				return FReply::Handled();
			}))
		.OnWheel(FPointerEventHandler::CreateLambda(
			[this](const FGeometry&, const FPointerEvent& Event) -> FReply
			{
				if (OnZoom)
				{
					OnZoom(Event.GetWheelDelta());
				}
				return FReply::Handled();
			}))
		.OnDoubleClick(FPointerEventHandler::CreateLambda(
			[this](const FGeometry&, const FPointerEvent&) -> FReply
			{
				if (OnResetView)
				{
					OnResetView();
				}
				return FReply::Handled();
			}));
}

// ---------------------------------------------------------------------- //
// thumbnails
// ---------------------------------------------------------------------- //
const FSlateBrush* STestPlatformUI::Thumb(const FString& Url, const FVector2D& DrawSize)
{
	const FString Key = FString::Printf(TEXT("%s|%.0fx%.0f"), *Url, DrawSize.X, DrawSize.Y);
	if (TSharedPtr<FSlateBrush>* Found = ThumbCache.Find(Key))
	{
		return Found->Get();
	}
	FetchThumb(Key, DrawSize);
	return nullptr;
}

void STestPlatformUI::FetchThumb(const FString& Key, const FVector2D& DrawSize)
{
	if (ThumbPending.Contains(Key) || !Api.IsValid())
	{
		return;
	}
	ThumbPending.Add(Key);
	// Key is "<url>|<w>x<h>": the browser needs the url only
	FString Url = Key;
	FString SizePart;
	Url.Split(TEXT("|"), &Url, &SizePart);
	Api->FetchBytes(Url, [WeakThis = TWeakPtr<STestPlatformUI>(SharedThis(this)), Key, DrawSize](
		bool bOk, const TArray<uint8>& Bytes)
	{
		TSharedPtr<STestPlatformUI> This = WeakThis.Pin();
		if (!This.IsValid())
		{
			return;
		}
		This->ThumbPending.Remove(Key);
		if (!bOk || Bytes.Num() == 0)
		{
			return;
		}
		UTexture2D* Texture = FImageUtils::ImportBufferAsTexture2D(Bytes);
		if (Texture == nullptr)
		{
			return;
		}
		Texture->SRGB = true;
		Texture->UpdateResource();
		This->ThumbTextures.Emplace(Texture); // keep it alive: the brush holds it raw
		TSharedPtr<FSlateBrush> ImageBrush = MakeShared<FSlateImageBrush>(Texture, DrawSize);
		This->ThumbCache.Add(Key, ImageBrush);
		This->RebuildMapRows();
		This->RebuildVehicles();
	});
}

#undef LOCTEXT_NAMESPACE
























