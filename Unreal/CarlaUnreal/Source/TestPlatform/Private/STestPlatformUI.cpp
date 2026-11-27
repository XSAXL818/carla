#include "STestPlatformUI.h"

#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
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
					SNew(SBox).WidthOverride(384.f)[MakeRightColumn()]
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
		// backend offline
		+ SOverlay::Slot()
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				const bool bUp = IsBackendReady();
				return (!bUp && bUIVisible) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				Card(SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[
						Text(FString::Printf(TEXT("配置后端未运行（%s）"),
							*FTestPlatformApi::RootUrl()), 14, true, ColWarn)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
					[
						Text(LOCTEXT("OfflineHint",
							"地图、路线与仿真会话都由它提供，启动后会自动接管。\n"
							"手动启动：Source/TestPlatform/Platform/backend 下运行 python run.py").ToString(), 11, false, ColMuted)
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 12.f, 0.f, 0.f))
					[
						SNew(SButton)
						.ButtonStyle(&BtnGhost())
						.ContentPadding(FMargin(16.f, 6.f))
						.OnClicked_Lambda([this]() { ReloadCatalog(); return FReply::Handled(); })
						[Text(LOCTEXT("Retry", "重试").ToString(), 11)]
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
					[SNew(SBorder).BorderImage(&Brush(ColAccent)).Padding(FMargin(3.f, 3.f))]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 0.f, 0.f))
					[
						SAssignNew(ServerChipText, STextBlock).Text(FText::FromString(TEXT("服务器 127.0.0.1 : 2000")))
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
				[Text(TEXT("同步模式 · 20 Hz"), 10, false, ColMuted)]
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

TSharedRef<SWidget> STestPlatformUI::SliderRow(const FString& Label, float Min, float Max,
	float Step, float& Value, const FString& Unit)
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)[Text(Label, 10, false, ColMuted)]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SAssignNew(SpeedValueText, STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("%.0f %s"), Value, *Unit)))
				.Font(F(10, true))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 1.f, 0.f, 0.f))
		[
			SNew(SSlider)
			.Value_Lambda([&Value, Min, Max]()
			{
				return (Max > Min) ? FMath::Clamp((Value - Min) / (Max - Min), 0.f, 1.f) : 0.f;
			})
			.OnValueChanged_Lambda([&Value, Min, Max, Step](float Normalized)
			{
				float NewValue = Min + Normalized * (Max - Min);
				if (Step > 0.f)
				{
					NewValue = FMath::RoundToFloat(NewValue / Step) * Step;
				}
				Value = FMath::Clamp(NewValue, Min, Max);
			})
		];
}

TSharedRef<SWidget> STestPlatformUI::CheckRow(const FString& Label, bool& Flag)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([&Flag]()
			{
				return Flag ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([&Flag](ECheckBoxState NewState)
			{
				Flag = NewState == ECheckBoxState::Checked;
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 0.f, 0.f))
		[Text(Label, 10, false, ColMuted)];
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
				RebuildAgent();
				RebuildSummary();
			}, Algo.Desc)
		];
	}
	AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))[AlgWrap];

	AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
	[
		Text(LOCTEXT("AlgoNote",
			"参数由算法自描述：/algorithms 返回字段 schema（类型 / 范围 / 单位 / 默认值）。"
			"面板控件按内置算法固定，新增算法或参数需同步更新面板。").ToString(), 9, false, ColDim)
	];

	// behaviour chips straight from the schema enum
	TSharedRef<SWrapBox> BehaviourWrap = SNew(SWrapBox).UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(6.f, 6.f));
	if (const FAlgoInfo* Algo = Api->SelectedAlgorithm(Config.AlgoId))
	{
		for (const FAlgoParam& Param : Algo->Params)
		{
			if (Param.Type != TEXT("enum") || Param.Key != TEXT("behavior"))
			{
				continue;
			}
			for (const FAlgoOption& Option : Param.Options)
			{
				const FString Value = Option.Value;
				BehaviourWrap->AddSlot()
				[
					Chip(Option.Label, Value == Config.Behavior, [this, Value]()
					{
						Config.Behavior = Value;
						RebuildAgent();
					})
				];
			}
		}
	}
	AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 8.f, 0.f))
		[Text(LOCTEXT("Behaviour", "驾驶风格").ToString(), 10, false, ColMuted)]
		+ SHorizontalBox::Slot().FillWidth(1.f)[BehaviourWrap]
	];

	AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
	[SliderRow(LOCTEXT("TargetSpeed", "目标速度").ToString(), 10.f, 130.f, 5.f,
		Config.TargetSpeedKmh, TEXT("km/h"))];

	AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 6.f, 0.f, 0.f))
	[CheckRow(LOCTEXT("IgnoreLights", "忽略红绿灯").ToString(), Config.bIgnoreTrafficLights)];
	AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 3.f, 0.f, 0.f))
	[CheckRow(LOCTEXT("IgnoreVehicles", "忽略其他车辆").ToString(), Config.bIgnoreVehicles)];
	AgentBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 3.f, 0.f, 0.f))
	[CheckRow(LOCTEXT("IgnoreSigns", "忽略停车标志").ToString(), Config.bIgnoreStopSigns)];
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
				Text(LOCTEXT("SumHint",
					"路线按 xodr 生成（road_id / lane_id / s），启动前会做一次 Waypoint 解析校验。").ToString(),
					9, false, ColWarn)
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

	Api->PreviewMap(Config, [this, Done](bool bOk, const FSessionStatus& Status, const FString& Error)
	{
		bLoadingMap = false;
		bBusy = false;
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
	Api->StartSession(SessionId, [this](bool bOk, const FSessionStatus& Status, const FString& Error)
	{
		bBusy = false;
		if (bOk)
		{
			Session = Status;
			SetUIVisible(false); // the world takes the screen
			SetStatus(LOCTEXT("Running3", "运行中").ToString());
		}
		else
		{
			SetStatus(FString::Printf(TEXT("启动失败：%s"), *Error), true);
		}
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

void STestPlatformUI::ReloadCatalog()
{
	if (Api.IsValid())
	{
		Api->LoadCatalog();
	}
	bListsBuilt = false;
}

bool STestPlatformUI::IsBackendReady() const
{
	return Api.IsValid() && Api->IsReady();
}

void STestPlatformUI::UpdateChrome()
{
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(StatusDetail));
		StatusText->SetColorAndOpacity(FSlateColor(bWarnStatus ? ColWarn : ColMuted));
	}
	if (SessionChipText.IsValid())
	{
		SessionChipText->SetText(FText::FromString(FString::Printf(
			TEXT("会话 · %s"), *StateText(Session.State))));
		SessionChipText->SetColorAndOpacity(FSlateColor(StateColor()));
	}
	if (ServerChipText.IsValid())
	{
		ServerChipText->SetText(FText::FromString(IsBackendReady()
			? TEXT("服务器 127.0.0.1 : 2000 · 已连接")
			: TEXT("服务器 127.0.0.1 : 2000 · 未连接")));
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
	if (SpeedValueText.IsValid())
	{
		SpeedValueText->SetText(FText::FromString(
			FString::Printf(TEXT("%.0f km/h"), Config.TargetSpeedKmh)));
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
	bListsBuilt = true;

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
	const bool bReady = Api->IsReady();
	if (bReady && !bListsBuilt)
	{
		RebuildAll();
	}
	// a blocking LoadMap can outlive its request (engine-side freeze): never
	// leave the UI stuck in "loading" forever
	if (bLoadingMap && LoadStartedAt > 0.0 &&
		FPlatformTime::Seconds() - LoadStartedAt > 300.0)
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
	if (bReady != bLastBackendReady)
	{
		bLastBackendReady = bReady;
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
























