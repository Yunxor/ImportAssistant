#include "SImportAssistantWindow.h"

#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Brushes/SlateNoResource.h"
#include "Rendering/DrawElements.h"
#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IContentBrowserSingleton.h"
#include "IDetailsView.h"
#include "PropertyEditorModule.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "Input/DragAndDrop.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Settings/EditorLoadingSavingSettings.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Images/SImage.h"


#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"

#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

using namespace ImportAssistant;

namespace
{
struct FOptionalResultColumn
{
    FName Id;
    const TCHAR* Label;
    const TCHAR* Description;
    float Width;
};

const FOptionalResultColumn OptionalResultColumns[] =
{
    {TEXT("Type"), TEXT("类型"), TEXT("资产的 UE 类型，名称与配色来自官方资产类型定义。单元格提示显示完整类路径、.uasset 格式及源文件扩展名。"), 0.12f},
    {TEXT("Path"), TEXT("资产路径"), TEXT("UE 资产包路径，不是磁盘上的 FBX 源文件路径。单元格提示显示完整路径。"), 0.16f},
    {TEXT("Source"), TEXT("原来源"), TEXT("检索时资产记录的原始导入来源，可能是相对路径。不同于本次拖入的 FBX；报告保留该来源快照。"), 0.16f},
    {TEXT("Reason"), TEXT("匹配依据"), TEXT("本次检索命中的源路径、路径映射或名称关键词，多种依据取并集。关键词或映射候选不等于已确认同源；单元格提示展示实际命中内容。"), 0.12f},
    {TEXT("AutoReimport"), TEXT("自动重导入"), TEXT("按当前活动目录、过滤规则及资产真实来源关联评估 UE 原生自动重导入条件；不要求 .uasset 目录受监控。")
        TEXT("\n已启用不表示正在更新或更新成功；需确认模式不是静默执行，原生流程可能独立保存。关键词和插件路径映射不建立原生来源关联。")
        TEXT("\nUE 不公开实际运行时过滤规则及重置完成信号，繁忙或第三方同目录覆盖时需核实。单元格提示展示当前设置、监控范围及关联依据。"), 0.13f},
    {TEXT("Dirty"), TEXT("是否脏数据"), TEXT("动画或关联 Skeleton 所在包是否存在 UE 未保存修改，不表示数据损坏。单元格提示列出具体脏资产。")
        TEXT("\n列表检查不加载资产；执行前会加载所选动画准确复核，确认后允许覆盖已有修改但不自动保存。"), 0.12f},
    {TEXT("VersionTime"), TEXT("版本时间"), TEXT("上次导入时记录的源 FBX 修改时间（UTC），不是重导入操作时间，也不是 uasset 保存时间。")
        TEXT("\n只读取资产导入元数据，不用当前磁盘源文件时间代替。未保存资产显示当前内存导入记录。")
        TEXT("\n按本行来源槽位显示；单元格提示列出各来源路径与时间，缺失记录显示未知。"), 0.16f},
    {TEXT("State"), TEXT("状态 / 结果"), TEXT("该行的选择或最近一次执行结果。已更新，未保存表示仅修改内存，不代表资产已保存。")
        TEXT("\n报告保留期间重新勾选不覆盖结果文字；单元格提示显示完整结果及失败原因。"), 0.16f}
};

struct FGroupFrameEdges
{
    float TopInset = 0;
    bool bTop = false;
    bool bBottom = false;
};

FGroupFrameEdges GetGroupFrameEdges(const FResultNode& Node, bool bExpanded)
{
    return {Node.bGroup ? 8.0f : 0.0f, Node.bGroup, Node.bGroup ? !bExpanded : Node.bLastInGroup};
}

// 标题、正文和组尾分别画边，跨虚拟化行形成同一个完整边框，避免嵌套列表破坏列宽和滚动。
template<typename RowBase>
class TImportAssistantFramedRow : public RowBase
{
public:
    void SetFrameNode(FResultNodePtr Node) { FrameNode = MoveTemp(Node); }
    void SetTypeTint(FLinearColor Tint) { TypeTint = Tint.CopyWithNewOpacity(0.12f); }

protected:
    virtual const FSlateBrush* GetBorder() const override
    {
        static const FSlateNoResource NoBackground;
        return &NoBackground;
    }

    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect,
        FSlateWindowElementList& Elements, int32 LayerId, const FWidgetStyle& Style, bool bParentEnabled) const override
    {
        if (!FrameNode) { return RowBase::OnPaint(Args, Geometry, CullingRect, Elements, LayerId, Style, bParentEnabled); }
        const FGroupFrameEdges Edges = GetGroupFrameEdges(*FrameNode, FrameNode->bGroup && this->IsItemExpanded());
        const FVector2f Size = Geometry.GetLocalSize();
        const float Height = Size.Y - Edges.TopInset;
        const ESlateDrawEffect Effect = this->ShouldBeEnabled(bParentEnabled) ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;
        const FSlateBrush* White = FAppStyle::GetBrush(TEXT("WhiteBrush"));
        const FVector2f Offset(0, Edges.TopInset);
        FSlateDrawElement::MakeBox(Elements, LayerId, Geometry.ToPaintGeometry(FVector2f(Size.X, Height), FSlateLayoutTransform(Offset)),
            White, Effect, FLinearColor(0.035f, 0.035f, 0.035f) * Style.GetColorAndOpacityTint());
        if (FrameNode->Row)
        {
            FSlateDrawElement::MakeBox(Elements, LayerId + 1, Geometry.ToPaintGeometry(FVector2f(Size.X, Height), FSlateLayoutTransform(Offset)),
                White, Effect, TypeTint * Style.GetColorAndOpacityTint());
        }
        const int32 ContentLayer = RowBase::OnPaint(Args, Geometry, CullingRect, Elements, LayerId + 2, Style, bParentEnabled);
        const int32 FrameLayer = ContentLayer + 1;
        const FLinearColor BorderColor = FLinearColor(0.18f, 0.18f, 0.18f) * Style.GetColorAndOpacityTint();
        FSlateDrawElement::MakeBox(Elements, FrameLayer, Geometry.ToPaintGeometry(FVector2f(1, Height), FSlateLayoutTransform(Offset)), White, Effect, BorderColor);
        FSlateDrawElement::MakeBox(Elements, FrameLayer, Geometry.ToPaintGeometry(FVector2f(1, Height),
            FSlateLayoutTransform(FVector2f(Size.X - 1, Edges.TopInset))), White, Effect, BorderColor);
        if (Edges.bTop)
        {
            FSlateDrawElement::MakeBox(Elements, FrameLayer, Geometry.ToPaintGeometry(FVector2f(Size.X, 1),
                FSlateLayoutTransform(Offset)), White, Effect, BorderColor);
        }
        if (Edges.bBottom)
        {
            FSlateDrawElement::MakeBox(Elements, FrameLayer, Geometry.ToPaintGeometry(FVector2f(Size.X, 1),
                FSlateLayoutTransform(FVector2f(0, Size.Y - 1))), White, Effect, BorderColor);
        }
        return FrameLayer;
    }

private:
    FResultNodePtr FrameNode;
    FLinearColor TypeTint = FLinearColor::Transparent;
};

class SImportAssistantGroupRow : public TImportAssistantFramedRow<STableRow<FResultNodePtr>>
{
public:
    using FArguments = STableRow<FResultNodePtr>::FArguments;
    void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& Owner)
    {
        STableRow<FResultNodePtr>::Construct(Args, Owner);
    }
};

class SImportAssistantRow : public TImportAssistantFramedRow<SMultiColumnTableRow<FResultNodePtr>>
{
public:
    SLATE_BEGIN_ARGS(SImportAssistantRow) {}
        SLATE_ARGUMENT(FRowPtr, Item)
        SLATE_ARGUMENT(FResultNodePtr, Node)
        SLATE_ARGUMENT(FAssetTypePresentation, Presentation)
        SLATE_ARGUMENT(TFunction<void(bool)>, Select)
        SLATE_ARGUMENT(TFunction<bool()>, CanEdit)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& Owner)
    {
        Item = Args._Item;
        Presentation = Args._Presentation;
        SetFrameNode(Args._Node);
        SetTypeTint(Presentation.Color);
        Select = Args._Select;
        CanEdit = Args._CanEdit;
        SMultiColumnTableRow::Construct(FSuperRowType::FArguments().Padding(0), Owner);
    }

    virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
    {
        TSharedRef<SWidget> CellContent = SNullWidget::NullWidget;
        if (Column == TEXT("Select"))
        {
            CellContent = SNew(SCheckBox).IsChecked_Lambda([this] { return Item->bSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .IsEnabled_Lambda([this] { return Item->CanSelect() && CanEdit(); })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Select(State == ECheckBoxState::Checked); });
        }
        else if (Column == TEXT("Asset"))
        {
            CellContent = SNew(SButton).HAlign(HAlign_Fill).ButtonColorAndOpacity(Presentation.Color)
                .ToolTipText(FText::FromString(Item->Asset->ObjectPath.ToString() + TEXT("\nSkeleton：") + Item->Asset->Skeleton))
                .OnClicked_Lambda([this]
                {
                    const FAssetData Asset = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
                        .GetAssetByObjectPath(Item->Asset->ObjectPath);
                    FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<FAssetData>{ Asset });
                    return FReply::Handled();
                })
                [SNew(STextBlock).Text(FText::FromString(Item->Asset->Name)).Justification(ETextJustify::Center)
                    .OverflowPolicy(ETextOverflowPolicy::Ellipsis)];
        }
        else if (Column == TEXT("Type"))
        {
            CellContent = SNew(SBorder).BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush"))).BorderBackgroundColor(Presentation.Color)
                .Padding(FMargin(5, 2)).ToolTipText(DescribeAssetFileTypes(*Item->Asset, Presentation))
                [SNew(STextBlock).Text(Presentation.Label).ColorAndOpacity(FLinearColor::White).OverflowPolicy(ETextOverflowPolicy::Ellipsis)];
        }
        else if (Column == TEXT("AutoReimport"))
        {
            CellContent = SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Item->AutoReimport.Label); })
                .ToolTipText_Lambda([this] { return FText::FromString(Item->AutoReimport.Tooltip); }).OverflowPolicy(ETextOverflowPolicy::Ellipsis);
        }
        else if (Column == TEXT("Dirty"))
        {
            CellContent = SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Item->Dirty.Label); })
                .ToolTipText_Lambda([this] { return FText::FromString(Item->Dirty.Tooltip); }).OverflowPolicy(ETextOverflowPolicy::Ellipsis);
        }
        else if (Column == TEXT("VersionTime"))
        {
            CellContent = SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Item->VersionTime.Label); })
                .ToolTipText_Lambda([this] { return FText::FromString(Item->VersionTime.Tooltip); }).OverflowPolicy(ETextOverflowPolicy::Ellipsis);
        }
        else if (Column == TEXT("State"))
        {
            CellContent = SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Item->Status); })
                .ToolTipText_Lambda([this] { return FText::FromString(Item->Status); }).OverflowPolicy(ETextOverflowPolicy::Ellipsis);
        }
        else
        {
            FString Value;
            if (Column == TEXT("Path")) { Value = Item->Asset->PackageName; }
            else if (Column == TEXT("Reason"))
            {
                Value = DescribeReason(Item->Reason);
                if (Item->bOutsideTemplate) { Value += TEXT(" · 模板目录外候选"); }
            }
            else if (Column == TEXT("Source"))
            {
                TArray<FString> Sources;
                for (const FSourceRecord& Source : Item->Asset->Sources) { Sources.Add(Source.Original); }
                Value = FString::Join(Sources, TEXT(" | "));
            }
            CellContent = SNew(STextBlock).Text(FText::FromString(Value)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                .ToolTipText(FText::FromString(Column == TEXT("Reason") ? Item->ReasonTooltip : Value));
        }
        // 表格会重设最外层裁剪；内层固定裁剪内容，确保长文本不会侵占单元格之间的留白。
        return SNew(SBox).Padding(FMargin(6, 3)).VAlign(VAlign_Center)
            [SNew(SBox).Clipping(EWidgetClipping::ClipToBounds)[CellContent]];
    }
private:
    FRowPtr Item;
    FAssetTypePresentation Presentation;
    TFunction<void(bool)> Select;
    TFunction<bool()> CanEdit;
};
}

void SImportAssistantWindow::Construct(const FArguments& Args)
{
    Settings.Reset(NewObject<UImportAssistantSettings>());
    bUseSavedSettings = Args._LoadSavedSettings;
    if (bUseSavedSettings) { Settings->LoadPluginConfig(ConfigError); }
    Index = MakeUnique<FAssetIndex>();
    ResultHeader = SNew(SHeaderRow)
        + SHeaderRow::Column(TEXT("Select")).FixedWidth(32).ShouldGenerateWidget(true).HAlignHeader(HAlign_Center).VAlignHeader(VAlign_Center)
            [CreateSelectionCheckBox()]
        + SHeaderRow::Column(TEXT("Asset")).DefaultLabel(FText::FromString(TEXT("资产（点击定位）"))).FillWidth(0.15f).ShouldGenerateWidget(true)
            .DefaultTooltip(FText::FromString(TEXT("匹配的动画资产名称，点击可在内容浏览器中定位。单元格提示显示资产对象路径及关联 Skeleton。")));
    for (const FOptionalResultColumn& Column : OptionalResultColumns)
    {
        ResultHeader->AddColumn(SHeaderRow::Column(Column.Id).DefaultLabel(FText::FromString(Column.Label))
            .DefaultTooltip(FText::FromString(Column.Description)).FillWidth(Column.Width));
    }
    FDetailsViewArgs DetailsArgs;
    DetailsArgs.bAllowSearch = false;
    DetailsArgs.bHideSelectionTip = true;
    DetailsArgs.bShowOptions = false;
    DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
    SettingsView = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor")).CreateDetailView(DetailsArgs);
    SettingsView->SetObject(Settings.Get());
    SettingsView->SetIsPropertyEditingEnabledDelegate(FIsPropertyEditingEnabled::CreateLambda([this] { return !Queue.IsRunning() && !bPreparingUpdate; }));
    SettingsView->OnFinishedChangingProperties().AddSP(this, &SImportAssistantWindow::OnSettingsEdited);
    // 原生监控延迟至少五秒且仅在空闲态重建；首次打开也留出观察窗口，过滤规则仍按当前配置评估。
    AutoReimportSettingsSettleUntil = FPlatformTime::Seconds() + 6.0;
    AutoReimportSettingsHandle = GetMutableDefault<UEditorLoadingSavingSettings>()->OnSettingChanged().AddSP(
        this, &SImportAssistantWindow::OnAutoReimportSettingsChanged);
    DirtyStateHandle = UPackage::PackageDirtyStateChangedEvent.AddLambda([this](UPackage*) { bDirtyStatusChanged = true; });
    auto Editable = [this] { return !Queue.IsRunning() && !bPreparingUpdate; };
    ChildSlot
    [
        SNew(SBorder).Padding(10)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
            [SNew(STextBlock).Text(FText::FromString(TEXT("Import Assistant — 拖入 FBX，审阅同源动画后再更新")))]
            + SVerticalBox::Slot().FillHeight(1)
            [
                SNew(SSplitter)
                + SSplitter::Slot().Value(0.28f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(4)
                    [SNew(STextBlock).Text(FText::FromString(TEXT("设置（编辑完成后自动保存到插件）")))
                        .ToolTipText(FText::FromString(Settings->GetPluginConfigFilename()))]
                    + SVerticalBox::Slot().FillHeight(1)[SettingsView.ToSharedRef()]
                    + SVerticalBox::Slot().AutoHeight().Padding(4)
                    [SNew(STextBlock).Text_Lambda([this] { return FText::FromString(ConfigError); }).AutoWrapText(true)
                        .ColorAndOpacity(FLinearColor(1.0f, 0.4f, 0.2f))]
                ]
                + SSplitter::Slot().Value(0.72f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
                    [
                        SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6, 4))
                        + SWrapBox::Slot()
                        [SNew(SButton).Text(FText::FromString(TEXT("添加 FBX…"))).IsEnabled_Lambda(Editable).OnClicked(this, &SImportAssistantWindow::Browse)]
                        + SWrapBox::Slot()
                        [SNew(SButton).Text(FText::FromString(TEXT("清空本次文件"))).IsEnabled_Lambda(Editable).OnClicked_Lambda([this]
                            {
                                Files.Reset(); AutoReimportFiles.Reset(); FileScopes.Reset(); Rows.Reset(); SelectedCount = 0; UnknownCount = 0; UnmatchedFiles.Reset();
                                UpdateAutoReimportRows();
                                RebuildGroups(false); SettingsChanged(); return FReply::Handled();
                            })]
                        + SWrapBox::Slot()
                        [SNew(SButton).Text(FText::FromString(TEXT("检索 / 刷新"))).IsEnabled_Lambda(Editable)
                            .ToolTipText(FText::FromString(TEXT("重新匹配当前 FBX。范围未变化时复用索引并更新内存资产信息；范围变化时自动重建索引。")))
                            .OnClicked(this, &SImportAssistantWindow::Search)]
                        + SWrapBox::Slot()
                        [SNew(SButton).Text(FText::FromString(TEXT("重建索引"))).IsEnabled_Lambda(Editable)
                            .ToolTipText(FText::FromString(TEXT("从 UE 资产注册表重新构建插件的元数据索引，再执行匹配。用于排查插件缓存不一致；不强制重扫磁盘，也不重导入资产。")))
                            .OnClicked_Lambda([this] { ActiveRoots.Reset(); return Search(); })]
                        + SWrapBox::Slot()
                        [SNew(SButton).Text(FText::FromString(TEXT("停止匹配"))).IsEnabled_Lambda([this] { return SearchTask.IsValid() || bSearchRequested; })
                            .ToolTipText(FText::FromString(TEXT("取消当前匹配，过期结果不可用于更新。不取消资产重导入，也不强行中断 UE 资产发现。")))
                            .OnClicked_Lambda([this] { SettingsChanged(); Notice = TEXT("已取消匹配，旧结果不可执行；可重新检索。"); return FReply::Handled(); })]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 5)
                    [
                        SNew(SWrapBox).Tag(TEXT("ImportAssistant.ResultActions")).UseAllottedSize(true).HAlign(HAlign_Right)
                        .InnerSlotPadding(FVector2D(6, 4))
                        + SWrapBox::Slot()
                        [SNew(SButton).Text(FText::FromString(TEXT("选择明确同源项"))).IsEnabled_Lambda([this] { return CanSelectResults(); })
                            .ToolTipText(FText::FromString(TEXT("勾选精确源路径命中且来源无冲突的资产；关键词和映射候选仍需自行审阅。")))
                            .OnClicked_Lambda([this]
                            {
                                for (const FRowPtr& Row : Rows)
                                {
                                    if (Row->CanSelect() && !Row->bFileConflict && EnumHasAnyFlags(Row->Reason, EMatchReason::Exact)) { SetSelected(Row, true); }
                                }
                                return FReply::Handled();
                            })]
                        + SWrapBox::Slot()
                        [SNew(SButton).Tag(TEXT("ImportAssistant.UpdateToggle")).Text(this, &SImportAssistantWindow::UpdateButtonText)
                            .ToolTipText(this, &SImportAssistantWindow::UpdateButtonTooltip).IsEnabled(this, &SImportAssistantWindow::CanToggleUpdate)
                            .OnClicked(this, &SImportAssistantWindow::ToggleUpdate)]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 3)
                    [
                        SNew(SBorder).BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder"))).Padding(6, 3)
                        [
                            SNew(SHorizontalBox).Tag(TEXT("ImportAssistant.ColumnToolbar"))
                            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                            [SNew(STextBlock).Text_Lambda([this]
                                { return FText::FromString(FString::Printf(TEXT("已添加 %d 个 FBX；按添加顺序分组，悬停组标题查看源文件路径"), Files.Num())); })
                                .OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                            [
                                SNew(SComboButton).Tag(TEXT("ImportAssistant.ColumnSettings"))
                                .ComboButtonStyle(FAppStyle::Get(), TEXT("SimpleComboButton")).ContentPadding(4).HasDownArrow(false)
                                .ToolTipText(FText::FromString(TEXT("显示列设置：勾选信息列，应用于所有 FBX 分组；不影响检索、选择和报告。")))
                                .OnGetMenuContent(this, &SImportAssistantWindow::BuildColumnMenu)
                                .ButtonContent()
                                [SNew(SImage).Image(FAppStyle::GetBrush(TEXT("Icons.Settings"))).DesiredSizeOverride(FVector2D(16, 16))]
                            ]
                        ]
                    ]
                    + SVerticalBox::Slot().FillHeight(1).Padding(0, 4)
                    [
                        SAssignNew(Tree, STreeView<FResultNodePtr>).TreeItemsSource(&Groups).SelectionMode(ESelectionMode::None)
                        .OnGenerateRow(this, &SImportAssistantWindow::GenerateRow).OnGetChildren(this, &SImportAssistantWindow::GetResultChildren)
                        .OnExpansionChanged_Lambda([this](FResultNodePtr Node, bool)
                        {
                            // 折叠时同一个标题行会被复用，显式刷新闭合边框，避免沿用展开状态的绘制缓存。
                            if (TSharedPtr<ITableRow> Row = Tree->WidgetFromItem(Node)) { Row->AsWidget()->Invalidate(EInvalidateWidgetReason::Paint); }
                        })
                        .HeaderRow(ResultHeader)
                    ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 0)
            [
                SNew(SBorder).Tag(TEXT("ImportAssistant.InformationArea")).Padding(8)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                        [SNew(STextBlock).Text(this, &SImportAssistantWindow::StatusText).AutoWrapText(true)]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
                        [SNew(SButton).Tag(TEXT("ImportAssistant.ExportReport")).Text(FText::FromString(TEXT("导出报告…")))
                            .ToolTipText(FText::FromString(TEXT("导出当前检索和更新结果，不更新或保存动画资产。")))
                            .IsEnabled_Lambda(Editable).OnClicked(this, &SImportAssistantWindow::Export)]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
                    [SNew(STextBlock).Text(FText::FromString(TEXT("更新仅修改内存，不自动保存或签出。请检查动画及 Skeleton 后自行保存；来源改绑或覆盖未保存修改时会提示确认。请避免同时开启自动重导入。")))
                        .AutoWrapText(true)]
                ]
            ]
        ]
    ];
    ApplyResultColumnVisibility();
}
TSharedRef<SWidget> SImportAssistantWindow::BuildColumnMenu()
{
    FMenuBuilder Menu(false, nullptr);
    Menu.BeginSection(TEXT("ResultColumns"), FText::FromString(TEXT("显示信息列（选择框和资产名称固定）")));
    for (const FOptionalResultColumn& Column : OptionalResultColumns)
    {
        Menu.AddMenuEntry(FText::FromString(Column.Label),
            FText::FromString(FString(Column.Description) + TEXT("\n显示或隐藏此列，对所有 FBX 分组生效。")), FSlateIcon(),
            FUIAction(FExecuteAction::CreateSP(this, &SImportAssistantWindow::ToggleResultColumn, Column.Id), FCanExecuteAction(),
                FIsActionChecked::CreateSP(this, &SImportAssistantWindow::IsResultColumnVisible, Column.Id)), NAME_None, EUserInterfaceActionType::ToggleButton);
    }
    Menu.EndSection();
    Menu.AddMenuSeparator();
    Menu.AddMenuEntry(FText::FromString(TEXT("恢复默认列")), FText::FromString(TEXT("显示全部信息列。")), FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(this, &SImportAssistantWindow::ResetResultColumns)));
    return Menu.MakeWidget();
}

bool SImportAssistantWindow::IsResultColumnVisible(FName ColumnId) const
{
    return ResultHeader->IsColumnVisible(ColumnId);
}

void SImportAssistantWindow::ToggleResultColumn(FName ColumnId)
{
    // 固定列不允许隐藏；配置只记录隐藏的信息列，新增列在旧配置中默认显示。
    bool bOptional = false;
    for (const FOptionalResultColumn& Column : OptionalResultColumns) { if (Column.Id == ColumnId) { bOptional = true; break; } }
    if (!bOptional) { return; }
    const bool bShow = !IsResultColumnVisible(ColumnId);
    if (bShow) { Settings->HiddenResultColumns.Remove(ColumnId); }
    else { Settings->HiddenResultColumns.AddUnique(ColumnId); }
    ResultHeader->SetShowGeneratedColumn(ColumnId, bShow);
    SaveSettings();
}

void SImportAssistantWindow::ResetResultColumns()
{
    Settings->HiddenResultColumns.Reset();
    ApplyResultColumnVisibility();
    SaveSettings();
}

void SImportAssistantWindow::ApplyResultColumnVisibility()
{
    // 仅刷新表头生成列，Slate 同步重排已生成资产行，不替换结果或重建分组。
    for (const FOptionalResultColumn& Column : OptionalResultColumns)
    {
        ResultHeader->SetShowGeneratedColumn(Column.Id, !Settings->HiddenResultColumns.Contains(Column.Id));
    }
}

void SImportAssistantWindow::SaveSettings()
{
    if (bUseSavedSettings) { Settings->SavePluginConfig(ConfigError); }
}

SImportAssistantWindow::~SImportAssistantWindow()
{
    UPackage::PackageDirtyStateChangedEvent.Remove(DirtyStateHandle);
    SettingsView->OnFinishedChangingProperties().RemoveAll(this);
    GetMutableDefault<UEditorLoadingSavingSettings>()->OnSettingChanged().Remove(AutoReimportSettingsHandle);
    if (CancelSearch) { CancelSearch->store(true); }
    if (SearchTask.IsValid()) { SearchTask.Wait(); }
}

void SImportAssistantWindow::SettingsChanged()
{
    bSettingsDirty = true;
    bSearchRequested = false;
    if (CancelSearch) { CancelSearch->store(true); }
}

void SImportAssistantWindow::AddFiles(const TArray<FString>& Added)
{
    for (const FString& File : Added)
    {
        if (FPaths::GetExtension(File).Equals(TEXT("fbx"), ESearchCase::IgnoreCase))
        {
            const FString Full = FPaths::ConvertRelativePathToFull(File);
            if (!Files.ContainsByPredicate([&Full](const FString& Existing) { return NormalizePath(Existing) == NormalizePath(Full); })) { Files.Add(Full); }
        }
    }
    Search();
}

FReply SImportAssistantWindow::OnDragOver(const FGeometry&, const FDragDropEvent& Event)
{
    const TSharedPtr<FExternalDragOperation> Drag = Event.GetOperationAs<FExternalDragOperation>();
    return !Queue.IsRunning() && Drag && Drag->HasFiles() ? FReply::Handled() : FReply::Unhandled();
}

FReply SImportAssistantWindow::OnDrop(const FGeometry&, const FDragDropEvent& Event)
{
    const TSharedPtr<FExternalDragOperation> Drag = Event.GetOperationAs<FExternalDragOperation>();
    if (!Queue.IsRunning() && Drag && Drag->HasFiles()) { AddFiles(Drag->GetFiles()); return FReply::Handled(); }
    return FReply::Unhandled();
}

FReply SImportAssistantWindow::Browse()
{
    TArray<FString> Selected;
    if (IDesktopPlatform* Platform = FDesktopPlatformModule::Get())
    {
        if (Platform->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()), TEXT("选择动画 FBX"),
            TEXT(""), TEXT(""), TEXT("FBX (*.fbx)|*.fbx"), EFileDialogFlags::Multiple, Selected)) { AddFiles(Selected); }
    }
    return FReply::Handled();
}

FReply SImportAssistantWindow::Search()
{
    if (CancelSearch) { CancelSearch->store(true); }
    TArray<FName> Roots;
    TArray<FPathMapping> ParsedMappings;
    if (!Settings->Parse(Roots, ParsedMappings, Notice)) { bSettingsDirty = true; return FReply::Handled(); }
    SaveSettings();
    TArray<FFileSearchScope> NewScopes;
    TArray<FName> IndexRoots;
    if (!BuildStorageSearchPlan(Settings->StorageTemplates, Settings->bUseStorageTemplates, Settings->StorageSearchMode,
        EImportAssistantAssetCategory::Animation, Files, Roots, Settings->bRecursive, NewScopes, IndexRoots, Notice))
    {
        bSettingsDirty = true;
        return FReply::Handled();
    }
    FileScopes = MoveTemp(NewScopes);
    RefreshAutoReimport(true);
    bCheckTemplateDirectories = true;
    RebuildGroups(true);
    if (IndexRoots != ActiveRoots || Settings->bRecursive != bActiveRecursive)
    {
        ActiveRoots = IndexRoots;
        bActiveRecursive = Settings->bRecursive;
        Index->Configure(IndexRoots, Settings->bRecursive);
    }
    else { Index->RefreshMemory(); }
    Mappings = MoveTemp(ParsedMappings);
    bSettingsDirty = false;
    bShowReport = false;
    bSearchRequested = true;
    Notice = Files.IsEmpty() ? TEXT("请拖入或添加 FBX；未知来源将在检索结果中单独列出。") : TEXT("");
    return FReply::Handled();
}

void SImportAssistantWindow::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry, Time, Delta);
    if (bPreparingUpdate) { return; }
    Queue.Tick();
    if (FPlatformTime::Seconds() >= NextDirtyCheck && (bDirtyStatusChanged || DirtyAssetRevision != Index->GetRevision())) { RefreshDirtyAssets(); }
    if (bWasRunning && !Queue.IsRunning())
    {
        RefreshDirtyAssets();
        RefreshVersionTimes();
        bShowReport = true;
        Notice = Queue.GetStatus() + TEXT("；报告已保留。可继续勾选并更新，执行前会重新预检当前来源和脏状态；资产仍需你检查后自行保存。");
    }
    bWasRunning = Queue.IsRunning();
    if (Queue.IsRunning()) { return; }
    Index->Tick();
    if (Index->IsReady() && VersionAssetRevision != Index->GetRevision()) { RefreshVersionTimes(); }
    if (bAutoReimportSettingsDirty || FPlatformTime::Seconds() >= NextAutoReimportCheck) { RefreshAutoReimport(false); }
    if (bCheckTemplateDirectories && Index->IsReady()) { CheckTemplateDirectories(); }
    if (SearchTask.IsValid() && SearchTask.IsReady())
    {
        FSearchOutput Output = SearchTask.Get();
        SearchTask = {};
        if (!CancelSearch->load() && !bSettingsDirty && TaskRevision == Index->GetRevision() && Index->IsReady())
        {
            TMap<FString, FString> Selected;
            for (const FRowPtr& Row : Rows) { if (Row->bSelected) { Selected.Add(Row->Key(), Row->Asset->SourceJson); } }
            Rows = MoveTemp(Output.Rows);
            SelectionGroups.Reset();
            SelectedAssets.Reset();
            SelectedCount = 0;
            UnmatchedFiles = MoveTemp(Output.UnmatchedFiles);
            for (const FRowPtr& Row : Rows)
            {
                const FString* OldJson = Selected.Find(Row->Key());
                if (OldJson && *OldJson == Row->Asset->SourceJson && Row->CanSelect() && !Row->bFileConflict) { SetSelected(Row, true); }
            }
            UnknownCount = Output.UnknownCount;
            SearchSeconds = Output.Seconds;
            ResultRevision = TaskRevision;
            bSearchRequested = false;
            UpdateAutoReimportRows();
            RefreshDirtyAssets();
            RefreshVersionTimes();
            RebuildGroups(false);
        }
        else if (!bSettingsDirty && !bShowReport) { bSearchRequested = true; }
    }
    if (!SearchTask.IsValid() && !bSettingsDirty && !bShowReport && !Files.IsEmpty() && ResultRevision != Index->GetRevision()) { bSearchRequested = true; }
    if (bSearchRequested && !SearchTask.IsValid() && Index->IsReady() && !Files.IsEmpty())
    {
        bSearchRequested = false;
        TaskRevision = Index->GetRevision();
        CancelSearch = MakeShared<std::atomic_bool, ESPMode::ThreadSafe>(false);
        const auto Cancel = CancelSearch;
        const auto Snapshot = Index->Snapshot();
        const auto FileSnapshot = Files;
        const FMatchSettings SettingsSnapshot = Settings->MakeMatchSettings();
        const auto MappingSnapshot = Mappings;
        const auto ScopeSnapshot = FileScopes;
        SearchTask = Async(EAsyncExecution::ThreadPool, [Snapshot, FileSnapshot, SettingsSnapshot, MappingSnapshot, ScopeSnapshot, Cancel]()
        {
            return Match(*Snapshot, FileSnapshot, SettingsSnapshot, MappingSnapshot, *Cancel, ScopeSnapshot);
        });
    }
}

bool SImportAssistantWindow::CanSelectResults() const
{
    return !Queue.IsRunning() && !bPreparingUpdate && !bSettingsDirty && !bSearchRequested && !SearchTask.IsValid() &&
        !Files.IsEmpty() && Index->IsReady() && (bShowReport || ResultRevision == Index->GetRevision());
}

bool SImportAssistantWindow::CanExecute() const
{
    // 报告模式不依赖上批来源快照；真正执行仍需重新加载所选资产并定稿风险计划。
    return CanSelectResults() && SelectedCount > 0;
}

TSharedRef<SCheckBox> SImportAssistantWindow::CreateSelectionCheckBox(FResultNodePtr Group)
{
    return SNew(SCheckBox).Tag(Group ? TEXT("ImportAssistant.GroupSelection") : TEXT("ImportAssistant.GlobalSelection"))
        .IsChecked_Lambda([this, Group] { return GetSelectionState(Group); })
        .IsEnabled_Lambda([this, Group] { return CanChangeSelection(Group); })
        .OnCheckStateChanged_Lambda([this, Group](ECheckBoxState) { ToggleSelection(Group); })
        .ToolTipText(FText::FromString(Group ?
            TEXT("选择本组可选资产；部分或全部已选时点击取消本组选择。全选本组会取消其他 FBX 对同一资产的互斥选择，不改变分组展开状态。") :
            TEXT("选择所有分组中的可选资产，含折叠组；部分或全部已选时点击取消全部选择。多 FBX 来源冲突项需在组内或逐项明确选择。")));
}

ECheckBoxState SImportAssistantWindow::GetSelectionState(FResultNodePtr Group) const
{
    const int32 Selected = Group ? Group->SelectedCount : SelectedCount;
    const int32 Selectable = Group ? Group->SelectableCount : SelectableCount;
    if (Selected == 0) { return ECheckBoxState::Unchecked; }
    return Selected == Selectable ? ECheckBoxState::Checked : ECheckBoxState::Undetermined;
}

bool SImportAssistantWindow::CanChangeSelection(FResultNodePtr Group) const
{
    return CanSelectResults() && (Group ? Group->SelectableCount : SelectableCount) > 0;
}

void SImportAssistantWindow::ToggleSelection(FResultNodePtr Group)
{
    if (!CanChangeSelection(Group)) { return; }
    // 按点击前的汇总状态决定目标，不将横杠当成全选请求；折叠仅改变显示，不改变本次作用范围。
    const bool bSelect = GetSelectionState(Group) == ECheckBoxState::Unchecked;
    if (Group)
    {
        for (const FResultNodePtr& Child : Group->Children)
        {
            if (Child->Row && Child->Row->CanSelect()) { SetSelected(Child->Row, bSelect); }
        }
    }
    else
    {
        TSet<FSoftObjectPath> Skipped;
        for (const FRowPtr& Row : Rows)
        {
            if (!Row->CanSelect()) { continue; }
            if (bSelect && Row->bFileConflict) { Skipped.Add(Row->Asset->ObjectPath); continue; }
            SetSelected(Row, bSelect);
        }
        Notice = Skipped.IsEmpty() ? FString() : FString::Printf(
            TEXT("全选已跳过 %d 个存在多 FBX 来源冲突的资产；请在对应 FBX 小组内或逐项选择来源。横杠表示仍有可选行未勾选。"), Skipped.Num());
    }
}

void SImportAssistantWindow::SetSelected(FRowPtr Row, bool bSelected)
{
    if (bSelected && !Row->CanSelect()) { return; }
    if (Row->bSelected == bSelected) { return; }
    if (bSelected)
    {
        // 已选资产按对象路径索引，组内全选遇到互斥来源时无需反复遍历全部结果。
        if (const FRowPtr Other = SelectedAssets.FindRef(Row->Asset->ObjectPath); Other && Other != Row)
        {
            SetSelected(Other, false);
            if (!bShowReport) { Other->Status = TEXT("已选择另一 FBX 来源"); }
        }
        SelectedAssets.Add(Row->Asset->ObjectPath, Row);
    }
    else { SelectedAssets.Remove(Row->Asset->ObjectPath); }
    const int32 Delta = bSelected ? 1 : -1;
    SelectedCount += Delta;
    if (const FResultNodePtr Group = SelectionGroups.FindRef(NormalizePath(Row->File))) { Group->SelectedCount += Delta; }
    Row->bSelected = bSelected;
    if (!bShowReport && Row->CanSelect()) { Row->Status = bSelected ? TEXT("已选择") : TEXT("未选择"); }
}

TSharedRef<ITableRow> SImportAssistantWindow::GenerateRow(FResultNodePtr Node, const TSharedRef<STableViewBase>& Owner)
{
    if (Node->Row)
    {
        return SNew(SImportAssistantRow, Owner).Item(Node->Row).Node(Node).Presentation(AssetPresentation.Get(Node->Row->Asset->AssetClassPath))
            .Select([this, Row = Node->Row](bool bSelected) { SetSelected(Row, bSelected); }).CanEdit([this] { return CanSelectResults(); });
    }
    TSharedRef<SImportAssistantGroupRow> Row = SNew(SImportAssistantGroupRow, Owner)
        .Padding(Node->bGroup ? FMargin(6, 14, 6, 6) : FMargin(6));
    Row->SetFrameNode(Node);
    if (Node->bGroup)
    {
        const int32 Count = Node->Children.Num() == 1 && !Node->Children[0]->Row ? 0 : Node->Children.Num();
        Row->SetContent
        (
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
                [CreateSelectionCheckBox(Node)]
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                [SNew(STextBlock).Text(FText::FromString(Node->Title)).Font(FAppStyle::GetFontStyle(TEXT("BoldFont")))
                    .OverflowPolicy(ETextOverflowPolicy::Ellipsis).ToolTipText(FText::FromString(Node->Tooltip))]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%d 个资产"), Count)))]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
            [SNew(STextBlock).Text(FText::FromString(TEXT("自动重导入：") + Node->AutoReimport.Label))
                .ToolTipText(FText::FromString(Node->AutoReimport.Tooltip)).AutoWrapText(true)]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
            [SNew(STextBlock).Text(FText::FromString(Node->ScopeSummary)).ToolTipText(FText::FromString(Node->ScopeDetails))
                .AutoWrapText(true).Visibility(Node->ScopeSummary.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)]
        );
    }
    else
    {
        Row->SetContent(SNew(STextBlock).Text(FText::FromString(Node->Title)).ToolTipText(FText::FromString(Node->Tooltip)));
    }
    return Row;
}

void SImportAssistantWindow::GetResultChildren(FResultNodePtr Node, TArray<FResultNodePtr>& OutChildren) const
{
    OutChildren = Node->Children;
}

void SImportAssistantWindow::RebuildGroups(bool bPending)
{
    TMap<FString, bool> Expansion;
    for (const FResultNodePtr& Group : Groups) { Expansion.Add(Group->File, Tree->IsItemExpanded(Group)); }
    Groups = BuildResultGroups(Files, Rows, bPending, FileScopes, AutoReimportFiles);
    SelectionGroups.Reset();
    SelectedAssets.Reset();
    SelectedCount = 0;
    SelectableCount = 0;
    for (const FResultNodePtr& Group : Groups)
    {
        if (!Group->File.IsEmpty()) { SelectionGroups.Add(NormalizePath(Group->File), Group); }
        SelectableCount += Group->SelectableCount;
        SelectedCount += Group->SelectedCount;
        for (const FResultNodePtr& Child : Group->Children)
        {
            if (Child->Row && Child->Row->CanSelect() && Child->Row->bSelected) { SelectedAssets.Add(Child->Row->Asset->ObjectPath, Child->Row); }
        }
    }
    Tree->ClearExpandedItems();
    for (const FResultNodePtr& Group : Groups)
    {
        const bool* Expanded = Expansion.Find(Group->File);
        Tree->SetItemExpansion(Group, Expanded ? *Expanded : true);
    }
    Tree->RequestTreeRefresh();
}

void SImportAssistantWindow::UpdateAutoReimportRows()
{
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    for (const FRowPtr& Row : Rows)
    {
        const FAutoReimportInfo* FileInfo = AutoReimportFiles.Find(NormalizePath(Row->File));
        Row->AutoReimport = FileInfo ? EvaluateAutoReimportAsset(*Row->Asset, Row->File, *FileInfo) : FAutoReimportInfo();
        if (FileInfo && FileInfo->State == EAutoReimportState::Enabled)
        {
            // 结果记录保留原来源用于审阅；监控状态则读取当前元数据，避免改绑来源后仍显示旧关联。
            const FAssetData Latest = Registry.GetAssetByObjectPath(Row->Asset->ObjectPath);
            Row->AutoReimport = Latest.IsValid() ? EvaluateAutoReimportAsset(*MakeRecord(Latest, TEXT("")), Row->File, *FileInfo) : FAutoReimportInfo();
        }
    }
}

void SImportAssistantWindow::RefreshDirtyAssets()
{
    NextDirtyCheck = FPlatformTime::Seconds() + 0.5;
    bDirtyStatusChanged = false;
    DirtyAssetRevision = Index->GetRevision();
    // 定时更新值缓存，单元格和 Tooltip 求值不查找 UObject；共享同一资产的候选复用检查结果。
    TMap<FSoftObjectPath, FDirtyAssetInfo> Inspected;
    for (const FRowPtr& Row : Rows)
    {
        FDirtyAssetInfo* Info = Inspected.Find(Row->Asset->ObjectPath);
        if (!Info) { Info = &Inspected.Add(Row->Asset->ObjectPath, InspectDirtyAssets(*Row->Asset)); }
        Row->Dirty = *Info;
    }
}

void SImportAssistantWindow::RefreshVersionTimes()
{
    VersionAssetRevision = Index->GetRevision();
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TMap<FSoftObjectPath, FRecordPtr> Current;
    for (const FRowPtr& Row : Rows)
    {
        FRecordPtr* Record = Current.Find(Row->Asset->ObjectPath);
        if (!Record)
        {
            const FAssetData Latest = Registry.GetAssetByObjectPath(Row->Asset->ObjectPath);
            Record = &Current.Add(Row->Asset->ObjectPath, Latest.IsValid() ? MakeRecord(Latest, TEXT("")) : Row->Asset);
        }
        // 列展示当前元数据，报告中的原来源及本轮结果不因刷新或选择而被覆盖。
        Row->VersionTime = DescribeSourceVersion(**Record, Row->SourceIndex);
    }
}

void SImportAssistantWindow::OnAutoReimportSettingsChanged(FName PropertyName)
{
    if (PropertyName == GET_MEMBER_NAME_CHECKED(UEditorLoadingSavingSettings, bMonitorContentDirectories) ||
        PropertyName == GET_MEMBER_NAME_CHECKED(UEditorLoadingSavingSettings, AutoReimportDirectorySettings))
    {
        AutoReimportSettingsSettleUntil = FPlatformTime::Seconds() + 6.0;
    }
    bAutoReimportSettingsDirty = true;
}

void SImportAssistantWindow::RefreshAutoReimport(bool bForce)
{
    const double Now = FPlatformTime::Seconds();
    NextAutoReimportCheck = Now + 1.0;
    const bool bSettingsSettling = Now < AutoReimportSettingsSettleUntil;
    const auto Active = GetActiveAutoReimportDirectories();
    const bool bDirectoriesChanged = Active.IsSet() != LastMonitoredDirectories.IsSet() ||
        (Active.IsSet() && LastMonitoredDirectories.IsSet() && *Active != *LastMonitoredDirectories);
    const bool bAssetsChanged = AutoReimportAssetRevision != Index->GetRevision();
    if (!bForce && !bAutoReimportSettingsDirty && !bDirectoriesChanged && !bAssetsChanged &&
        AutoReimportSnapshot.bSettingsSettling == bSettingsSettling) { return; }
    AutoReimportSnapshot = CaptureAutoReimportSnapshot(bSettingsSettling);
    LastMonitoredDirectories = Active;
    AutoReimportAssetRevision = Index->GetRevision();
    bAutoReimportSettingsDirty = false;
    AutoReimportFiles.Reset();
    for (const FString& File : Files) { AutoReimportFiles.Add(NormalizePath(File), EvaluateAutoReimportFile(File, AutoReimportSnapshot)); }
    UpdateAutoReimportRows();
    RebuildGroups(bSearchRequested || SearchTask.IsValid() || bSettingsDirty || (!bShowReport && ResultRevision != Index->GetRevision()));
}

void SImportAssistantWindow::CheckTemplateDirectories()
{
    bCheckTemplateDirectories = false;
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    for (FFileSearchScope& Scope : FileScopes)
    {
        for (const FString& Root : Scope.TemplateRoots)
        {
            if (!Registry.PathExists(Root))
            {
                const FString Warning = TEXT("规则目标目录不存在或尚未被发现：") + Root;
                Scope.Summary += TEXT("\n") + Warning;
                Scope.Details += TEXT("\n\n") + Warning + TEXT("；该范围未经确认，零匹配不代表该目录不存在其他资产。");
            }
        }
    }
    RebuildGroups(true);
}

void SImportAssistantWindow::OnSettingsEdited(const FPropertyChangedEvent& Event)
{
    SaveSettings();
    SettingsChanged();
}
FText SImportAssistantWindow::StatusText() const
{
    FString Text = Queue.IsRunning() ? Queue.GetStatus() : Index->GetStatus();
    if (SearchTask.IsValid()) { Text += TEXT("；后台匹配中…"); }
    if (bSettingsDirty) { Text += TEXT("；配置已变更，请点击检索"); }
    if (FileScopes.ContainsByPredicate([](const FFileSearchScope& Scope) { return Scope.bLimitedToTemplates; }))
    {
        Text += TEXT("；仅模板目录模式：相关 FBX 未检查模板目录外资产");
    }
    Text += FString::Printf(TEXT("\n结果 %d 行，已选择 %d；未知来源 %d；上次匹配 %.3f 秒。%s"),
        Rows.Num(), SelectedCount, UnknownCount, SearchSeconds, *Notice);
    if (!UnmatchedFiles.IsEmpty()) { Text += TEXT("\n未找到匹配：") + FString::Join(UnmatchedFiles, TEXT("；")); }
    return FText::FromString(Text);
}

FText SImportAssistantWindow::UpdateButtonText() const
{
    if (Queue.IsStopRequested()) { return FText::FromString(TEXT("正在停止…")); }
    return FText::FromString(Queue.IsRunning() ? TEXT("停止更新") : TEXT("更新所选…"));
}

FText SImportAssistantWindow::UpdateButtonTooltip() const
{
    if (Queue.IsStopRequested())
    {
        return FText::FromString(TEXT("已请求停止，正在等待当前资产处理完成。已完成的修改不会撤销，也不会自动保存。"));
    }
    if (Queue.IsRunning())
    {
        return FText::FromString(TEXT("当前资产处理完成后停止后续更新。已完成的修改不会撤销，也不会自动保存；单项导入阻塞界面时需等界面恢复响应。"));
    }
    return FText::FromString(TEXT("更新已勾选资产，仅修改内存，不自动保存。仅来源改绑或覆盖已有未保存修改时确认；同源且干净时直接执行。"));
}

bool SImportAssistantWindow::CanToggleUpdate() const
{
    return Queue.IsRunning() ? !Queue.IsStopRequested() : CanExecute();
}

FReply SImportAssistantWindow::ToggleUpdate()
{
    if (Queue.IsRunning())
    {
        if (!Queue.IsStopRequested())
        {
            Queue.Cancel();
            Notice = TEXT("已请求停止更新；当前资产处理完成后取消剩余项目，已完成修改保留且不会自动保存。");
        }
        return FReply::Handled();
    }
    if (!CanToggleUpdate()) { return FReply::Handled(); }
    return Execute();
}

FReply SImportAssistantWindow::Execute()
{
    if (!CanExecute()) { return FReply::Handled(); }
    TGuardValue<bool> Preparing(bPreparingUpdate, true);
    // 精确预检仅加载本次选择；从计划定稿起持续跟踪修改，弹窗期间的新修改也必须重新确认。
    const auto Plan = BuildReimportPlan(Rows, bShowReport);
    RefreshDirtyAssets();
    RefreshVersionTimes();
    if (!Plan->Error.IsEmpty()) { Notice = TEXT("未开始更新：") + Plan->Error; return FReply::Handled(); }
    if (Plan->Items.IsEmpty()) { Notice = TEXT("请先勾选需要更新的资产。"); return FReply::Handled(); }
    RefreshAutoReimport(true);
    const FString AutoWarning = BuildAutoReimportWarning(Rows);
    if (!AutoWarning.IsEmpty() && FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(AutoWarning)) != EAppReturnType::Yes)
    {
        return FReply::Handled();
    }
    const FString RiskWarning = BuildReimportWarning(*Plan);
    if (!RiskWarning.IsEmpty())
    {
        FPlatformApplicationMisc::ClipboardCopy(*DescribeReimportPlan(*Plan));
        if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(RiskWarning)) != EAppReturnType::Yes) { return FReply::Handled(); }
    }
    FString Changed;
    if (!IsReimportPlanCurrent(*Plan, Changed)) { Notice = TEXT("未开始更新：") + Changed; return FReply::Handled(); }
    Queue.Start(Rows, Plan);
    bWasRunning = Queue.IsRunning();
    return FReply::Handled();
}

FReply SImportAssistantWindow::Export()
{
    RefreshAutoReimport(true);
    RefreshDirtyAssets();
    RefreshVersionTimes();
    TArray<FString> Paths;
    IDesktopPlatform* Platform = FDesktopPlatformModule::Get();
    if (!Platform || !Platform->SaveFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
        TEXT("导出更新报告"), FPaths::ProjectSavedDir(), TEXT("ImportAssistant.csv"), TEXT("CSV (*.csv)|*.csv"), 0, Paths))
    {
        return FReply::Handled();
    }
    auto Cell = [](FString Value)
    {
        Value.ReplaceInline(TEXT("\""), TEXT("\"\""));
        if (!Value.IsEmpty() && FString(TEXT("=+-@\t\r")).Contains(Value.Left(1))) { Value = TEXT("'") + Value; }
        return TEXT("\"") + Value + TEXT("\"");
    };
    TMap<FString, FString> ScopeReports;
    for (const FFileSearchScope& Scope : FileScopes) { ScopeReports.Add(NormalizePath(Scope.File), Scope.Details); }
    FString Csv = TEXT("FBX,资产,原来源,匹配依据,已选择,状态,目录规范,模板解析与检索范围,自动重导入,自动重导入详情,是否脏数据,脏数据详情,版本时间,版本时间详情\r\n");
    for (const FRowPtr& Row : Rows)
    {
        TArray<FString> Sources;
        for (const FSourceRecord& Source : Row->Asset->Sources) { Sources.Add(Source.Original); }
        Csv += FString::Join(TArray<FString>{ Cell(Row->File), Cell(Row->Asset->ObjectPath.ToString()), Cell(FString::Join(Sources, TEXT(" | "))),
            Cell(DescribeReason(Row->Reason)), Cell(Row->bSelected ? TEXT("是") : TEXT("否")), Cell(Row->Status), Cell(Row->bOutsideTemplate ? TEXT("模板目录外候选") : TEXT("")),
            Cell(ScopeReports.FindRef(NormalizePath(Row->File))), Cell(Row->AutoReimport.Label), Cell(Row->AutoReimport.Tooltip), Cell(Row->Dirty.Label), Cell(Row->Dirty.Tooltip), Cell(Row->VersionTime.Label), Cell(Row->VersionTime.Tooltip) }, TEXT(",")) + TEXT("\r\n");
    }
    for (const FString& File : UnmatchedFiles)
    {
        Csv += Cell(File) + TEXT(",\"\",\"\",\"\",\"否\",\"指定范围内未找到匹配\",\"\",") +
            Cell(ScopeReports.FindRef(NormalizePath(File))) + TEXT(",") + Cell(AutoReimportFiles.FindRef(NormalizePath(File)).Label) +
            TEXT(",") + Cell(AutoReimportFiles.FindRef(NormalizePath(File)).Tooltip) + TEXT(",\"\",\"无匹配资产\",\"\",\"无匹配资产\"\r\n");
    }
    Notice = FFileHelper::SaveStringToFile(Csv, *Paths[0], FFileHelper::EEncodingOptions::ForceUTF8) ? TEXT("报告已导出：") + Paths[0] : TEXT("报告写入失败");
    return FReply::Handled();
}












#if WITH_DEV_AUTOMATION_TESTS
#include "Framework/MultiBox/MultiBox.h"
#include "Layout/ArrangedChildren.h"
#include "Misc/AutomationTest.h"
#include "Widgets/SToolTip.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantCellLayoutTest, "ImportAssistant.Editor.CellSpacingAndTooltip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantCellLayoutTest::RunTest(const FString&)
{
    auto Asset = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Asset->Name = TEXT("M_Neutral_Run_Loop_WithALongName");
    Asset->AssetClassPath = FTopLevelAssetPath(TEXT("/Script/Engine.AnimSequence"));
    Asset->PackageName = TEXT("/Game/Characters/UEFN_Mannequin/Animations/LongPath/Run");
    Asset->Sources.Add({ TEXT("D:/Source/Characters/UEFN_Mannequin/Animations/LongPath/Run.fbx"), {} });
    FRowPtr Item = MakeShared<FMatchRow>();
    Item->Asset = Asset;
    Item->Reason = EMatchReason::Keyword;
    Item->ReasonTooltip = TEXT("名称关键词匹配：任意关键词\n命中关键词：Run、Loop\n匹配对象：M_Neutral_Run_Loop_WithALongName");
    Item->Status = TEXT("未选择");
    Item->AutoReimport = {EAutoReimportState::Enabled, TEXT("已启用（需确认）"), TEXT("来源关联受监控 FBX；包含：*.fbx"), true};
    TArray<FResultNodePtr> Nodes;
    const TSharedRef<STreeView<FResultNodePtr>> Owner = SNew(STreeView<FResultNodePtr>).TreeItemsSource(&Nodes)
        .OnGenerateRow_Lambda([](FResultNodePtr, const TSharedRef<STableViewBase>& Table) -> TSharedRef<ITableRow>
            { return SNew(STableRow<FResultNodePtr>, Table); })
        .OnGetChildren_Lambda([](FResultNodePtr, TArray<FResultNodePtr>&) {})
        .HeaderRow(SNew(SHeaderRow) + SHeaderRow::Column(TEXT("Path")).DefaultLabel(FText::FromString(TEXT("资产路径"))));
    FAssetPresentationCache Cache;
    const TSharedRef<SImportAssistantRow> Row = SNew(SImportAssistantRow, Owner).Item(Item).Presentation(Cache.Get(Asset->AssetClassPath)).Select([](bool) {}).CanEdit([] { return true; });
    for (const FName Column : {FName(TEXT("Select")), FName(TEXT("Asset")), FName(TEXT("Type")), FName(TEXT("Path")), FName(TEXT("Source")),
        FName(TEXT("Reason")), FName(TEXT("AutoReimport")), FName(TEXT("Dirty")), FName(TEXT("VersionTime")), FName(TEXT("State"))})
    {
        const TSharedRef<SWidget> Cell = Row->GenerateWidgetForColumn(Column);
        Cell->SlatePrepass();
        const TSharedRef<SWidget> ClipBox = Cell->GetChildren()->GetChildAt(0);
        const FVector2D CellSize = Cell->GetDesiredSize();
        const FVector2D InnerSize = ClipBox->GetDesiredSize();
        TestEqual(TEXT("单元格左右合计留白：") + Column.ToString(), float(CellSize.X - InnerSize.X), 12.0f);
        TestEqual(TEXT("单元格上下合计留白：") + Column.ToString(), float(CellSize.Y - InnerSize.Y), 6.0f);
        TestEqual(TEXT("内容独立裁剪以保护留白"), ClipBox->GetClipping(), EWidgetClipping::ClipToBounds);
        FArrangedChildren Arranged(EVisibility::Visible);
        Cell->ArrangeChildren(FGeometry::MakeRoot(FVector2f(160, float(CellSize.Y)), FSlateLayoutTransform()), Arranged);
        if (TestEqual(TEXT("单元格布局只包含内层内容"), Arranged.Num(), 1))
        {
            TestEqual(TEXT("受限列宽保留两侧留白"), float(Arranged[0].Geometry.GetLocalSize().X), 148.0f);
        }
        if (Column == TEXT("Type"))
        {
            const TSharedRef<SWidget> Label = ClipBox->GetChildren()->GetChildAt(0);
            const TSharedPtr<IToolTip> Tooltip = Label->GetToolTip();
            if (TestTrue(TEXT("类型列有文件格式 Tooltip"), Tooltip.IsValid()))
            {
                const FString Text = StaticCastSharedRef<SToolTip>(Tooltip->AsWidget())->GetTextTooltip().ToString();
                TestTrue(TEXT("类型列包含实际资产类和源扩展名"), Text.Contains(TEXT("AnimSequence")) && Text.Contains(TEXT(".uasset")) && Text.Contains(TEXT(".fbx")));
            }
        }
        if (Column == TEXT("AutoReimport"))
        {
            const TSharedRef<STextBlock> Label = StaticCastSharedRef<STextBlock>(ClipBox->GetChildren()->GetChildAt(0));
            TestEqual(TEXT("自动重导入列显示独立状态"), Label->GetText().ToString(), Item->AutoReimport.Label);
            const TSharedPtr<IToolTip> Tooltip = Label->GetToolTip();
            if (TestTrue(TEXT("自动重导入列有范围与来源 Tooltip"), Tooltip.IsValid()))
            {
                TestEqual(TEXT("Tooltip 绑定当前监控依据"),
                    StaticCastSharedRef<SToolTip>(Tooltip->AsWidget())->GetTextTooltip().ToString(), Item->AutoReimport.Tooltip);
                Item->AutoReimport.Label = TEXT("无法确认");
                Item->AutoReimport.Tooltip = TEXT("监控配置刚变更，等待刷新");
                TestEqual(TEXT("状态文本动态读取而非构造时快照"), Label->GetText().ToString(), Item->AutoReimport.Label);
                TestEqual(TEXT("Tooltip 跟随配置刷新"),
                    StaticCastSharedRef<SToolTip>(Tooltip->AsWidget())->GetTextTooltip().ToString(), Item->AutoReimport.Tooltip);
            }
        }
        if (Column == TEXT("VersionTime"))
        {
            const TSharedRef<STextBlock> Label = StaticCastSharedRef<STextBlock>(ClipBox->GetChildren()->GetChildAt(0));
            TestEqual(TEXT("版本时间列读取缓存"), Label->GetText().ToString(), Item->VersionTime.Label);
            const auto Tooltip = Label->GetToolTip();
            if (TestTrue(TEXT("版本列提供语义和来源 Tooltip"), Tooltip.IsValid()))
            {
                Item->VersionTime.Label = TEXT("2026-11-09 02:03:04 UTC");
                Item->VersionTime.Tooltip = TEXT("上次导入时记录的 FBX 时间，非操作时间");
                TestEqual(TEXT("版本时间动态读取"), Label->GetText().ToString(), Item->VersionTime.Label);
                TestEqual(TEXT("版本详情动态刷新"), StaticCastSharedRef<SToolTip>(Tooltip->AsWidget())->GetTextTooltip().ToString(), Item->VersionTime.Tooltip);
            }
        }
        if (Column == TEXT("Dirty"))
        {
            const TSharedRef<STextBlock> Label = StaticCastSharedRef<STextBlock>(ClipBox->GetChildren()->GetChildAt(0));
            TestEqual(TEXT("脏数据列读取值缓存"), Label->GetText().ToString(), Item->Dirty.Label);
            const auto Tooltip = Label->GetToolTip();
            if (TestTrue(TEXT("脏数据列有具体资产 Tooltip"), Tooltip.IsValid()))
            {
                Item->Dirty.Label = TEXT("是：骨骼");
                Item->Dirty.Tooltip = TEXT("Skeleton：/Game/Dirty/Skeleton.Skeleton");
                TestEqual(TEXT("脏状态文本动态更新"), Label->GetText().ToString(), Item->Dirty.Label);
                TestEqual(TEXT("脏资产 Tooltip 动态更新"), StaticCastSharedRef<SToolTip>(Tooltip->AsWidget())->GetTextTooltip().ToString(), Item->Dirty.Tooltip);
            }
        }
        if (Column == TEXT("Reason"))
        {
            const TSharedRef<SWidget> Text = ClipBox->GetChildren()->GetChildAt(0);
            const TSharedPtr<IToolTip> Tooltip = Text->GetToolTip();
            if (TestTrue(TEXT("匹配依据单元格提供 Tooltip"), Tooltip.IsValid()))
            {
                TestEqual(TEXT("Tooltip 显示匹配证据而非依据名称"),
                    StaticCastSharedRef<SToolTip>(Tooltip->AsWidget())->GetTextTooltip().ToString(), Item->ReasonTooltip);
            }
        }
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantGroupFrameTest, "ImportAssistant.Editor.GroupFrameEdges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantGroupFrameTest::RunTest(const FString&)
{
    FResultNode Header;
    Header.bGroup = true;
    FGroupFrameEdges Edges = GetGroupFrameEdges(Header, true);
    TestTrue(TEXT("展开组标题只画顶边，底边留给正文末尾"), Edges.bTop && !Edges.bBottom);
    TestEqual(TEXT("小组上方保持独立间距"), Edges.TopInset, 8.0f);
    Edges = GetGroupFrameEdges(Header, false);
    TestTrue(TEXT("折叠后标题形成完整封闭边框"), Edges.bTop && Edges.bBottom);
    FResultNode Body;
    Edges = GetGroupFrameEdges(Body, false);
    TestTrue(TEXT("正文中间只延续两侧，不产生内部横线"), !Edges.bTop && !Edges.bBottom && Edges.TopInset == 0);
    Body.bLastInGroup = true;
    Edges = GetGroupFrameEdges(Body, false);
    TestTrue(TEXT("组尾资产闭合底边"), !Edges.bTop && Edges.bBottom);
    FResultNode Empty;
    Empty.bLastInGroup = true;
    TestTrue(TEXT("零匹配占位项也封闭底边"), GetGroupFrameEdges(Empty, false).bBottom);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantUpdateControlTest, "ImportAssistant.Editor.UpdateToggleAndToolbar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantUpdateControlTest::RunTest(const FString&)
{
    const TSharedRef<SImportAssistantWindow> Window = SNew(SImportAssistantWindow).LoadSavedSettings(false);
    Window->SlatePrepass();
    TMap<FName, TSharedRef<SWidget>> Tagged;
    TFunction<void(TSharedRef<SWidget>)> Visit = [&Visit, &Tagged](TSharedRef<SWidget> Widget)
    {
        if (!Widget->GetTag().IsNone()) { Tagged.Add(Widget->GetTag(), Widget); }
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Index < Children->Num(); ++Index) { Visit(Children->GetChildAt(Index)); }
    };
    Visit(Window);
    TFunction<int32(TSharedRef<SWidget>)> CountButtons = [&CountButtons](TSharedRef<SWidget> Widget)
    {
        int32 Count = Widget->GetType() == FName(TEXT("SButton")) ? 1 : 0;
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Index < Children->Num(); ++Index) { Count += CountButtons(Children->GetChildAt(Index)); }
        return Count;
    };
    const TSharedRef<SWidget>* Actions = Tagged.Find(FName(TEXT("ImportAssistant.ResultActions")));
    const TSharedRef<SWidget>* Information = Tagged.Find(FName(TEXT("ImportAssistant.InformationArea")));
    const TSharedRef<SWidget>* Button = Tagged.Find(FName(TEXT("ImportAssistant.UpdateToggle")));
    const TSharedRef<SWidget>* Export = Tagged.Find(FName(TEXT("ImportAssistant.ExportReport")));
    if (!TestTrue(TEXT("操作区、信息区、合并按钮和报告按钮均存在"), Actions && Information && Button && Export)) { return false; }
    TestEqual(TEXT("结果区只保留明确同源按钮和更新停止按钮"), CountButtons(*Actions), 2);
    TestEqual(TEXT("信息区仅保留一个报告按钮"), CountButtons(*Information), 1);
    const TSharedRef<SWrapBox> ActionBox = StaticCastSharedRef<SWrapBox>(*Actions);
    const FGeometry ActionGeometry = FGeometry::MakeRoot(FVector2f(1200, 100), FSlateLayoutTransform());
    ActionBox->Tick(ActionGeometry, FPlatformTime::Seconds(), 0);
    Window->SlatePrepass();
    FArrangedChildren ActionLayout(EVisibility::Visible);
    ActionBox->ArrangeChildren(ActionGeometry, ActionLayout);
    if (TestEqual(TEXT("宽窗口两个操作按钮同排排列"), ActionLayout.Num(), 2))
    {
        const FVector2D LastPosition = ActionLayout[1].Geometry.LocalToAbsolute(FVector2f::ZeroVector);
        const FVector2D LastSize = ActionLayout[1].Geometry.GetLocalSize();
        TestTrue(TEXT("操作按钮位于右侧而非靠左排列"), FMath::IsNearlyEqual(LastPosition.X + LastSize.X, 1200.0, 0.1));
    }
    TestEqual(TEXT("空闲按钮文字"), Window->UpdateButtonText().ToString(), FString(TEXT("更新所选…")));
    TestFalse(TEXT("无有效检索选择时不能执行更新"), Window->CanToggleUpdate());
    Window->ToggleUpdate();
    TestFalse(TEXT("无选择点击不会进入运行状态或确认框"), Window->Queue.IsRunning());

    auto Record = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Record->ObjectPath = FSoftObjectPath(TEXT("/Game/__ImportAssistantToolbar/NotLoaded.NotLoaded"));
    FRowPtr Row = MakeShared<FMatchRow>();
    Row->Asset = Record;
    Row->File = TEXT("C:/ToolbarTest.fbx");
    Row->SourceIndex = 0;
    Row->bSelected = true;
    Window->Queue.Start({Row});
    Window->bWasRunning = true;
    Window->SlatePrepass();
    TestEqual(TEXT("更新开始后文字切为停止"), Window->UpdateButtonText().ToString(), FString(TEXT("停止更新")));
    TestTrue(TEXT("运行时不受检索按钮禁用条件影响，可以停止"), Window->CanToggleUpdate());
    TestTrue(TEXT("真正的按钮也保持可点击"), (*Button)->IsEnabled());
    TestFalse(TEXT("运行中不能导出结果"), (*Export)->IsEnabled());
    TestTrue(TEXT("停止 Tooltip 明确不回滚且不保存"), Window->UpdateButtonTooltip().ToString().Contains(TEXT("不会撤销，也不会自动保存")));
    StaticCastSharedRef<SButton>(*Button)->SimulateClick();
    Window->SlatePrepass();
    TestTrue(TEXT("同一个按钮确实向队列发送停止请求"), Window->Queue.IsStopRequested());
    TestEqual(TEXT("请求停止后提示等待"), Window->UpdateButtonText().ToString(), FString(TEXT("正在停止…")));
    TestFalse(TEXT("请求停止后不允许重复点击"), Window->CanToggleUpdate());
    TestFalse(TEXT("实际按钮也被禁用"), (*Button)->IsEnabled());
    Window->ToggleUpdate();
    TestTrue(TEXT("重复调用不会启动另一批次"), Window->Queue.IsStopRequested());
    Window->Tick(FGeometry(), FPlatformTime::Seconds(), 0);
    Window->SlatePrepass();
    TestFalse(TEXT("队列已停止"), Window->Queue.IsRunning());
    TestEqual(TEXT("结束后恢复更新入口文字"), Window->UpdateButtonText().ToString(), FString(TEXT("更新所选…")));
    TestFalse(TEXT("没有有效检索范围的虚拟窗口结束后仍不能更新"), Window->CanToggleUpdate());
    TestTrue(TEXT("结束后可以导出报告"), (*Export)->IsEnabled());
    TestEqual(TEXT("剩余项被标记取消而未加载虚拟测试资产"), Row->Status, FString(TEXT("用户取消，未执行")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantColumnVisibilityTest, "ImportAssistant.Editor.ColumnVisibilityAndToolbar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantColumnVisibilityTest::RunTest(const FString&)
{
    const TSharedRef<SImportAssistantWindow> Window = SNew(SImportAssistantWindow).LoadSavedSettings(false);
    Window->SlatePrepass();
    TSharedPtr<SWidget> Toolbar, Gear;
    TFunction<void(TSharedRef<SWidget>)> Visit = [&Visit, &Toolbar, &Gear](TSharedRef<SWidget> Widget)
    {
        if (Widget->GetTag() == FName(TEXT("ImportAssistant.ColumnToolbar"))) { Toolbar = Widget; }
        if (Widget->GetTag() == FName(TEXT("ImportAssistant.ColumnSettings"))) { Gear = Widget; }
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Index < Children->Num(); ++Index) { Visit(Children->GetChildAt(Index)); }
    };
    Visit(Window);
    if (!TestTrue(TEXT("列表上方有工具栏和列设置齿轮"), Toolbar.IsValid() && Gear.IsValid())) { return false; }
    for (float Width : {1000.0f, 240.0f})
    {
        FArrangedChildren Layout(EVisibility::Visible);
        Toolbar->ArrangeChildren(FGeometry::MakeRoot(FVector2f(Width, 32), FSlateLayoutTransform()), Layout);
        if (TestEqual(TEXT("工具栏为说明与齿轮两项"), Layout.Num(), 2))
        {
            TestTrue(TEXT("窄宽窗口齿轮均贴右且不被说明挤出"),
                FMath::IsNearlyEqual(Layout[1].Geometry.GetAbsolutePosition().X + Layout[1].Geometry.GetLocalSize().X, double(Width), 0.1));
            TestTrue(TEXT("最右侧确实是齿轮按钮"), Layout[1].Widget == Gear);
        }
    }
    TestTrue(TEXT("齿轮显示原生设置图标且 Tooltip 可用"), Gear->GetToolTip().IsValid());

    auto Asset = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Asset->Name = TEXT("Run");
    Asset->AssetClassPath = FTopLevelAssetPath(TEXT("/Script/Engine.AnimSequence"));
    Asset->ObjectPath = FSoftObjectPath(TEXT("/Game/ColumnTest/Run.Run"));
    Asset->Sources.Add({TEXT("C:/ColumnTest/Run.fbx"), {}});
    FRowPtr Item = MakeShared<FMatchRow>();
    Item->Asset = Asset;
    Item->File = TEXT("C:/ColumnTest/Run.fbx");
    Item->SourceIndex = 0;
    Item->bSelected = true;
    Item->Status = TEXT("已选择");
    Window->Files = {Item->File, TEXT("C:/ColumnTest/Empty.fbx")};
    Window->Rows = {Item};
    Window->SelectedCount = 1;
    Window->RebuildGroups(false);
    const auto BeforeGroups = Window->Groups;
    Window->Tree->SetItemExpansion(Window->Groups[0], true);
    Window->Tree->SetItemExpansion(Window->Groups[1], false);
    Window->bSettingsDirty = false;
    Window->bSearchRequested = true;
    Window->CancelSearch = MakeShared<std::atomic_bool, ESPMode::ThreadSafe>(false);
    const uint64 Revision = Window->Index->GetRevision();
    const TSharedRef<SImportAssistantRow> Row = SNew(SImportAssistantRow, Window->Tree.ToSharedRef()).Item(Item)
        .Node(Window->Groups[0]->Children[0]).Presentation(Window->AssetPresentation.Get(Asset->AssetClassPath)).Select([](bool) {}).CanEdit([] { return true; });
    const TSharedRef<SWidget> Cells = Row->GetChildren()->GetChildAt(0);
    TestEqual(TEXT("默认资产行生成全部信息列"), Cells->GetChildren()->Num(), int32(UE_ARRAY_COUNT(OptionalResultColumns)) + 2);

    const TSharedRef<SMultiBoxWidget> Menu = StaticCastSharedRef<SMultiBoxWidget>(Window->BuildColumnMenu());
    TestFalse(TEXT("勾选时菜单保持打开，允许连续切换多列"), Menu->GetMultiBox()->ShouldCloseWindowAfterMenuSelection());
    TArray<FUIAction> Actions;
    for (const auto& Block : Menu->GetMultiBox()->GetBlocks())
    {
        if (Block->GetType() == EMultiBlockType::MenuEntry) { Actions.Add(Block->GetDirectActions()); }
    }
    if (!TestEqual(TEXT("菜单包含信息列及恢复默认，不含固定列"), Actions.Num(), int32(UE_ARRAY_COUNT(OptionalResultColumns)) + 1)) { return false; }
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(OptionalResultColumns); ++Index)
    {
        const FName Id = OptionalResultColumns[Index].Id;
        TestTrue(TEXT("信息列默认显示：") + Id.ToString(), Window->IsResultColumnVisible(Id));
        TestTrue(TEXT("勾选状态取自当前表头"), Actions[Index].GetCheckState() == ECheckBoxState::Checked);
        Actions[Index].Execute();
        TestFalse(TEXT("菜单动作实际隐藏表头：") + Id.ToString(), Window->ResultHeader->IsColumnGenerated(Id));
        TestEqual(TEXT("隐藏后资产单元格同步减少，不保留空列"), Cells->GetChildren()->Num(), int32(UE_ARRAY_COUNT(OptionalResultColumns)) + 1 - Index);
        TestTrue(TEXT("隐藏列写入内存配置"), Window->Settings->HiddenResultColumns.Contains(Id));
        TestTrue(TEXT("同一个菜单动态取消勾选"), Actions[Index].GetCheckState() == ECheckBoxState::Unchecked);
    }
    Row->SlatePrepass();
    FArrangedChildren CellLayout(EVisibility::Visible);
    Cells->ArrangeChildren(FGeometry::MakeRoot(FVector2f(1000, 32), FSlateLayoutTransform()), CellLayout);
    if (TestEqual(TEXT("全部信息列隐藏后仅保留两个固定单元格"), CellLayout.Num(), 2))
    {
        TestTrue(TEXT("选择列保持原固定宽度"), FMath::IsNearlyEqual(CellLayout[0].Geometry.GetLocalSize().X, 32.0, 0.1));
        TestTrue(TEXT("资产名称填满剩余宽度，没有隐藏列占位"), FMath::IsNearlyEqual(CellLayout[1].Geometry.GetLocalSize().X, 968.0, 0.1));
    }
    Window->ToggleResultColumn(TEXT("Asset"));
    Window->ToggleResultColumn(TEXT("Select"));
    Window->ResultHeader->SetShowGeneratedColumn(TEXT("Asset"), false);
    Window->ResultHeader->SetShowGeneratedColumn(TEXT("Select"), false);
    TestTrue(TEXT("选择框与资产名不能被隐藏"), Window->IsResultColumnVisible(TEXT("Select")) && Window->IsResultColumnVisible(TEXT("Asset")));
    Actions.Last().Execute();
    TestTrue(TEXT("恢复默认清空隐藏配置"), Window->Settings->HiddenResultColumns.IsEmpty());
    TestEqual(TEXT("恢复默认同步重建所有单元格"), Cells->GetChildren()->Num(), int32(UE_ARRAY_COUNT(OptionalResultColumns)) + 2);
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(OptionalResultColumns); ++Index)
    {
        TestTrue(TEXT("原菜单恢复已勾选状态"), Actions[Index].GetCheckState() == ECheckBoxState::Checked);
    }
    Actions[0].Execute();
    Actions[0].Execute();
    TestTrue(TEXT("同一列可再次勾选显示"), Window->IsResultColumnVisible(OptionalResultColumns[0].Id));
    Window->Settings->HiddenResultColumns = {TEXT("Source"), TEXT("Asset"), TEXT("RemovedOldColumn")};
    Window->ApplyResultColumnVisibility();
    TestFalse(TEXT("重开时应用隐藏列表"), Window->IsResultColumnVisible(TEXT("Source")));
    TestTrue(TEXT("旧配置或无效列名不影响固定列与新增列"), Window->IsResultColumnVisible(TEXT("Asset")) && Window->IsResultColumnVisible(TEXT("AutoReimport")));
    TestTrue(TEXT("列显隐不替换分组、不会丢失结果"), Window->Groups == BeforeGroups && Window->Rows.Num() == 1 && Window->Rows[0] == Item);
    TestTrue(TEXT("展开与折叠状态保持"), Window->Tree->IsItemExpanded(Window->Groups[0]) && !Window->Tree->IsItemExpanded(Window->Groups[1]));
    TestTrue(TEXT("选择状态和结果文案保持"), Item->bSelected && Window->SelectedCount == 1 && Item->Status == TEXT("已选择"));
    TestTrue(TEXT("不使检索失效、不取消后台匹配或修改索引"), !Window->bSettingsDirty && Window->bSearchRequested &&
        !Window->CancelSearch->load() && Window->Index->GetRevision() == Revision);
    TestFalse(TEXT("测试窗口关闭配置写入，不污染插件配置"), Window->bUseSavedSettings);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantColumnTooltipTest, "ImportAssistant.Editor.ColumnHeaderRemarks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantColumnTooltipTest::RunTest(const FString&)
{
    const auto Window = SNew(SImportAssistantWindow).LoadSavedSettings(false);
    Window->SlatePrepass();
    const auto Header = Window->ResultHeader;
    const auto& Columns = Header->GetColumns();
    TestEqual(TEXT("表头保留两固定列及全部信息列"), Columns.Num(), int32(UE_ARRAY_COUNT(OptionalResultColumns)) + 2);
    TestEqual(TEXT("资产名表头说明点击定位"), Columns[Header->FindColumnIndex(TEXT("Asset"))].DefaultTooltip.Get().ToString(),
        FString(TEXT("匹配的动画资产名称，点击可在内容浏览器中定位。单元格提示显示资产对象路径及关联 Skeleton。")));
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(OptionalResultColumns); ++Index)
    {
        const FOptionalResultColumn& Definition = OptionalResultColumns[Index];
        const int32 ColumnIndex = Header->FindColumnIndex(Definition.Id);
        if (!TestEqual(TEXT("信息列顺序不变：") + Definition.Id.ToString(), ColumnIndex, Index + 2)) { return false; }
        const SHeaderRow::FColumn& Column = Columns[ColumnIndex];
        TestEqual(TEXT("所有信息列表头均提供字段备注：") + Definition.Id.ToString(), Column.DefaultTooltip.Get().ToString(), FString(Definition.Description));
        TestEqual(TEXT("字段备注改动不改变列宽：") + Definition.Id.ToString(), Column.GetWidth(), Definition.Width);
    }
    const FString VersionRemark = Columns[Header->FindColumnIndex(TEXT("VersionTime"))].DefaultTooltip.Get().ToString();
    const FString DirtyRemark = Columns[Header->FindColumnIndex(TEXT("Dirty"))].DefaultTooltip.Get().ToString();
    const FString AutoRemark = Columns[Header->FindColumnIndex(TEXT("AutoReimport"))].DefaultTooltip.Get().ToString();
    TestTrue(TEXT("版本时间解释迁移至表头，含读取和保存边界"), VersionRemark.Contains(TEXT("上次导入时记录的源 FBX")) &&
        VersionRemark.Contains(TEXT("不是重导入操作时间")) && VersionRemark.Contains(TEXT("当前内存导入记录")));
    TestTrue(TEXT("脏状态表头说明未保存而非损坏"), DirtyRemark.Contains(TEXT("未保存修改")) && DirtyRemark.Contains(TEXT("不表示数据损坏")));
    TestTrue(TEXT("自动重导入表头保留条件、独立保存和公开规则边界"), AutoRemark.Contains(TEXT("真实来源关联")) &&
        AutoRemark.Contains(TEXT("独立保存")) && AutoRemark.Contains(TEXT("重置完成信号")));

    // 核对原生表头实际生成的 Tooltip，而不只检查声明数据；隐藏及恢复列也应保留备注。
    TArray<FString> RenderedRemarks;
    TFunction<void(TSharedRef<SWidget>)> Visit = [&Visit, &RenderedRemarks](TSharedRef<SWidget> Widget)
    {
        if (Widget->GetType() == FName(TEXT("STableColumnHeader")))
        {
            const auto Border = Widget->GetChildren()->GetChildAt(0);
            if (const auto Tooltip = Border->GetToolTip(); Tooltip.IsValid())
            {
                RenderedRemarks.Add(StaticCastSharedRef<SToolTip>(Tooltip->AsWidget())->GetTextTooltip().ToString());
            }
        }
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Index < Children->Num(); ++Index) { Visit(Children->GetChildAt(Index)); }
    };
    Visit(Header.ToSharedRef());
    for (const FOptionalResultColumn& Definition : OptionalResultColumns)
    {
        TestTrue(TEXT("悬停实际表头可读取字段备注：") + Definition.Id.ToString(), RenderedRemarks.Contains(FString(Definition.Description)));
    }
    Window->ToggleResultColumn(TEXT("VersionTime"));
    RenderedRemarks.Reset();
    Visit(Header.ToSharedRef());
    TestFalse(TEXT("隐藏的版本列不再生成悬停项"), RenderedRemarks.Contains(VersionRemark));
    Window->ToggleResultColumn(TEXT("VersionTime"));
    RenderedRemarks.Reset();
    Visit(Header.ToSharedRef());
    TestTrue(TEXT("恢复显示后备注仍正确绑定"), RenderedRemarks.Contains(VersionRemark));

    FAssetRecord Asset;
    Asset.ObjectPath = FSoftObjectPath(TEXT("/Game/Tooltip/Run.Run"));
    Asset.Sources.Add({TEXT("C:/Tooltip/Run.fbx"), {}, FDateTime(2026, 11, 9, 3, 4, 5)});
    const auto Details = DescribeSourceVersion(Asset, 0);
    TestTrue(TEXT("版本单元格只显示具体资产与记录"), Details.Tooltip.StartsWith(TEXT("资产：/Game/Tooltip/Run.Run")) &&
        Details.Tooltip.Contains(TEXT("C:/Tooltip/Run.fbx")) && Details.Tooltip.Contains(TEXT("2026-11-09 03:04:05 UTC")));
    TestFalse(TEXT("版本单元格不再重复字段含义"), Details.Tooltip.Contains(TEXT("不是重导入操作时间")) || Details.Tooltip.Contains(TEXT("只读取资产导入元数据")));
    TestTrue(TEXT("仅修改列备注不影响配置或选择"), Window->Settings->HiddenResultColumns.IsEmpty() && Window->SelectedCount == 0);
    return true;
}
#endif












