#include "ImportAssistantAutoReimport.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "ImportAssistantMatcher.h"
#include "ImportAssistantResultGroups.h"
#include "SImportAssistantWindow.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorFramework/AssetImportData.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

using namespace ImportAssistant;

namespace
{
struct FMonitorFixture
{
    TStrongObjectPtr<UEditorLoadingSavingSettings> Settings;
    FString Root;
    FString File;
    TOptional<TArray<FString>> Active;

    FMonitorFixture() : Settings(NewObject<UEditorLoadingSavingSettings>())
    {
        Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ImportAssistantTests") /
            FGuid::NewGuid().ToString(EGuidFormats::Digits) / TEXT("Monitored"));
        IFileManager::Get().MakeDirectory(*(Root / TEXT("Excluded")), true);
        File = Root / TEXT("Run.fbx");
        Active = TArray<FString>{Root};
        Settings->bMonitorContentDirectories = true;
        Settings->bPromptBeforeAutoImporting = false;
        Settings->bDetectChangesOnStartup = true;
        Settings->AutoReimportDirectorySettings.Reset();
        FAutoReimportDirectoryConfig& Config = Settings->AutoReimportDirectorySettings.AddDefaulted_GetRef();
        Config.SourceDirectory = Root;
        Config.MountPoint = TEXT("/Game/");
    }

    FAutoReimportSnapshot Capture() const { return BuildAutoReimportSnapshot(*Settings, Active, TEXT("fbx;png;")); }
    void AddFilter(const FString& Pattern, bool bInclude)
    {
        FAutoReimportWildcard& Filter = Settings->AutoReimportDirectorySettings[0].Wildcards.AddDefaulted_GetRef();
        Filter.Wildcard = Pattern;
        Filter.bInclude = bInclude;
    }
};

FRowPtr MakeRow(const FString& File, EAutoReimportState State, bool bSelected = true)
{
    auto Asset = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Asset->ObjectPath = FSoftObjectPath(TEXT("/Game/AutoReimport/Run.Run"));
    FRowPtr Row = MakeShared<FMatchRow>();
    Row->Asset = Asset;
    Row->File = File;
    Row->SourceIndex = 0;
    Row->bSelected = bSelected;
    Row->AutoReimport.State = State;
    return Row;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoReimportFiltersTest, "ImportAssistant.AutoReimport.DirectoryFilters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAutoReimportFiltersTest::RunTest(const FString&)
{
    FMonitorFixture Fixture;
    TestEqual(TEXT("实际活动目录和配置均符合时 FBX 受监控"), EvaluateAutoReimportFile(Fixture.File, Fixture.Capture()).State, EAutoReimportState::Enabled);
    TestEqual(TEXT("路径必须有目录边界，不能命中相似目录"), EvaluateAutoReimportFile(Fixture.Root + TEXT("Extra/Run.fbx"), Fixture.Capture()).State,
        EAutoReimportState::Disabled);
    TestEqual(TEXT("非工厂支持的源格式被排除"), EvaluateAutoReimportFile(Fixture.Root / TEXT("Run.unknown"), Fixture.Capture()).State, EAutoReimportState::Excluded);
    Fixture.AddFilter(TEXT("*.fbx"), true);
    Fixture.AddFilter(TEXT("Excluded/*"), false);
    TestEqual(TEXT("符合包含规则"), EvaluateAutoReimportFile(Fixture.File, Fixture.Capture()).State, EAutoReimportState::Enabled);
    const auto Excluded = EvaluateAutoReimportFile(Fixture.Root / TEXT("Excluded/Run.fbx"), Fixture.Capture());
    TestEqual(TEXT("排除优先于包含"), Excluded.State, EAutoReimportState::Excluded);
    TestEqual(TEXT("规范化相对段后再应用过滤规则"), EvaluateAutoReimportFile(Fixture.Root / TEXT("Unused/../Excluded/Run.fbx"), Fixture.Capture()).State,
        EAutoReimportState::Excluded);
    TestEqual(TEXT("离开被排除子目录的规范路径正常命中"), EvaluateAutoReimportFile(Fixture.Root / TEXT("Excluded/../Run.fbx"), Fixture.Capture()).State,
        EAutoReimportState::Enabled);
    const auto Settling = BuildAutoReimportSnapshot(*Fixture.Settings, Fixture.Active, TEXT("fbx;png;"), true);
    const auto SettlingInfo = EvaluateAutoReimportFile(Fixture.File, Settling);
    TestTrue(TEXT("根目录没变但过滤规则待刷新时不能报启用"), SettlingInfo.State == EAutoReimportState::Unknown &&
        SettlingInfo.Tooltip.Contains(TEXT("等待原生监控刷新")));
    TestTrue(TEXT("提示含命中目录和过滤规则"), Excluded.Tooltip.Contains(TEXT("Excluded/*")) && Excluded.Tooltip.Contains(Fixture.Root));
    TestEqual(TEXT("有包含规则时未命中的其他格式不自动放行"), EvaluateAutoReimportFile(Fixture.Root / TEXT("Run.png"), Fixture.Capture()).State,
        EAutoReimportState::Excluded);
    Fixture.Settings->bMonitorContentDirectories = false;
    TestEqual(TEXT("总开关关闭始终未启用"), EvaluateAutoReimportFile(Fixture.File, Fixture.Capture()).State, EAutoReimportState::Disabled);
    Fixture.Settings->bMonitorContentDirectories = true;
    Fixture.Active = TArray<FString>{};
    TestEqual(TEXT("配置已改但实际监控尚未生效不能称为已启用"), EvaluateAutoReimportFile(Fixture.File, Fixture.Capture()).State, EAutoReimportState::Unknown);
    Fixture.Active.Reset();
    TestEqual(TEXT("未取得实际监控列表时不猜测已启用"), EvaluateAutoReimportFile(Fixture.File, Fixture.Capture()).State, EAutoReimportState::Unknown);
    Fixture.Settings->AutoReimportDirectorySettings.Reset();
    Fixture.Active = TArray<FString>{Fixture.Root};
    TestEqual(TEXT("运行时目录过滤规则不可见时只报无法确认"), EvaluateAutoReimportFile(Fixture.File, Fixture.Capture()).State, EAutoReimportState::Unknown);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoReimportVirtualRootTest, "ImportAssistant.AutoReimport.VirtualAndOverlappingRoots",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAutoReimportVirtualRootTest::RunTest(const FString&)
{
    FMonitorFixture Fixture;
    Fixture.Settings->AutoReimportDirectorySettings[0].SourceDirectory = TEXT("/Game/");
    Fixture.Settings->AutoReimportDirectorySettings[0].MountPoint.Reset();
    const FString Content = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    Fixture.Active = TArray<FString>{Content};
    TestEqual(TEXT("/Game 通过引擎语义转换成物理源目录"), EvaluateAutoReimportFile(Content / TEXT("Any/Run.fbx"), Fixture.Capture()).State,
        EAutoReimportState::Enabled);
    Fixture.Settings->AutoReimportDirectorySettings.Reset();
    FAutoReimportDirectoryConfig Child;
    Child.SourceDirectory = Fixture.Root / TEXT("Excluded");
    Child.MountPoint = TEXT("/Game/");
    FAutoReimportDirectoryConfig Parent;
    Parent.SourceDirectory = Fixture.Root;
    Parent.MountPoint = TEXT("/Game/");
    FAutoReimportWildcard Exclude;
    Exclude.Wildcard = TEXT("Excluded/*");
    Exclude.bInclude = false;
    Parent.Wildcards.Add(Exclude);
    Fixture.Settings->AutoReimportDirectorySettings = {Child, Parent};
    Fixture.Active = TArray<FString>{Fixture.Root};
    const auto Snapshot = Fixture.Capture();
    TestEqual(TEXT("后面的父目录覆盖前面的子目录设置"), Snapshot.Directories.Num(), 1);
    TestEqual(TEXT("不能误用被父目录取代的子目录过滤规则"), EvaluateAutoReimportFile(Fixture.Root / TEXT("Excluded/Run.fbx"), Snapshot).State,
        EAutoReimportState::Excluded);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoReimportAssociationTest, "ImportAssistant.AutoReimport.SourceAssociation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAutoReimportAssociationTest::RunTest(const FString&)
{
    FMonitorFixture Fixture;
    FAssetRecord Asset;
    Asset.ObjectPath = FSoftObjectPath(TEXT("/Game/UnmonitoredAssetFolder/Run.Run"));
    Asset.PackageName = TEXT("/Game/UnmonitoredAssetFolder/Run");
    Asset.Sources.Add({Fixture.File, {}});
    const auto FileInfo = EvaluateAutoReimportFile(Fixture.File, Fixture.Capture());
    TestEqual(TEXT("uasset 目录不在监控范围也可因来源自动更新"), EvaluateAutoReimportAsset(Asset, Fixture.File, FileInfo).State, EAutoReimportState::Enabled);
    Fixture.Settings->bPromptBeforeAutoImporting = true;
    const auto PromptFile = EvaluateAutoReimportFile(Fixture.File, Fixture.Capture());
    const auto PromptAsset = EvaluateAutoReimportAsset(Asset, Fixture.File, PromptFile);
    TestTrue(TEXT("需确认模式不会被称为静默更新"), PromptAsset.bPrompt && PromptAsset.Label.Contains(TEXT("需确认")) &&
        PromptAsset.Tooltip.Contains(TEXT("启动时检测变更")));
    TestTrue(TEXT("监控单元格保留具体设置，通用边界移至表头"), PromptAsset.Tooltip.Contains(TEXT("处理前确认：需要确认")) &&
        !PromptAsset.Tooltip.Contains(TEXT("独立的保存流程")) && !PromptAsset.Tooltip.Contains(TEXT("不要求 .uasset")));
    FString PackageFile;
    FPackageName::TryConvertLongPackageNameToFilename(Asset.PackageName, PackageFile);
    FString Relative = Fixture.File;
    FString PackageDirectory = FPaths::ConvertRelativePathToFull(FPaths::GetPath(PackageFile)) + TEXT("/");
    FPaths::MakePathRelativeTo(Relative, *PackageDirectory);
    Asset.Sources = {{Relative, {}}};
    TestEqual(TEXT("支持包相对来源路径"), EvaluateAutoReimportAsset(Asset, Fixture.File, FileInfo).State, EAutoReimportState::Enabled);
    Asset.Sources = {{TEXT("C:/Old/Run.fbx"), {NormalizePath(Fixture.File)}}};
    const auto Unrelated = EvaluateAutoReimportAsset(Asset, Fixture.File, FileInfo);
    TestEqual(TEXT("候选路径或映射命中不能伪造原生来源关联"), Unrelated.State, EAutoReimportState::Unrelated);
    TestTrue(TEXT("来源不关联时提供实际记录便于诊断"), Unrelated.Tooltip.Contains(TEXT("C:/Old/Run.fbx")));
    Asset.Sources.Reset();
    TestEqual(TEXT("缺来源时无法确认"), EvaluateAutoReimportAsset(Asset, Fixture.File, FileInfo).State, EAutoReimportState::Unknown);
    Asset.Sources = {{TEXT("C:/Old/Run.fbx"), {}}, {Fixture.File, {}}};
    TestEqual(TEXT("遍历所有来源记录，不只第一项"), EvaluateAutoReimportAsset(Asset, Fixture.File, FileInfo).State, EAutoReimportState::Enabled);
    Fixture.Settings->bMonitorContentDirectories = false;
    TestEqual(TEXT("即使来源关联，总开关关闭也不报启用"), EvaluateAutoReimportAsset(Asset, Fixture.File, EvaluateAutoReimportFile(Fixture.File, Fixture.Capture())).State,
        EAutoReimportState::Disabled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutoReimportSummaryTest, "ImportAssistant.AutoReimport.SelectionWarningAndGroups",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAutoReimportSummaryTest::RunTest(const FString&)
{
    FMonitorFixture Fixture;
    const auto FileInfo = EvaluateAutoReimportFile(Fixture.File, Fixture.Capture());
    FRowPtr Enabled = MakeRow(Fixture.File, EAutoReimportState::Enabled);
    FRowPtr Disabled = MakeRow(Fixture.File, EAutoReimportState::Disabled);
    FRowPtr Unknown = MakeRow(Fixture.File, EAutoReimportState::Unknown);
    TestTrue(TEXT("只有选中且明确启用的行触发提示"), !BuildAutoReimportWarning({Enabled, Disabled, Unknown}).IsEmpty());
    Enabled->bSelected = false;
    TestTrue(TEXT("启用但未勾选的行不会触发警告"), BuildAutoReimportWarning({Enabled, Disabled, Unknown}).IsEmpty());
    Enabled->bSelected = true;
    Enabled->bSourceAmbiguous = true;
    TestTrue(TEXT("不能更新的歧义行不产生误导警告"), BuildAutoReimportWarning({Enabled}).IsEmpty());
    Enabled->bSourceAmbiguous = false;
    TestTrue(TEXT("部分启用统计"), SummarizeAutoReimport(FileInfo, {Enabled, Disabled}, false).Label.Contains(TEXT("部分启用（1 / 2")));
    TestTrue(TEXT("全部启用统计"), SummarizeAutoReimport(FileInfo, {Enabled}, false).Label.StartsWith(TEXT("已启用（1 / 1")));
    TestTrue(TEXT("未检索结束不预先宣称组内资产启用"), SummarizeAutoReimport(FileInfo, {Enabled}, true).State == EAutoReimportState::Unknown);
    TestTrue(TEXT("零匹配仍区分 FBX 监控与资产关联"), SummarizeAutoReimport(FileInfo, {}, false).Label.Contains(TEXT("未找到匹配资产")));
    const auto Groups = BuildResultGroups({Fixture.File}, {Enabled, Disabled}, false, {}, {{NormalizePath(Fixture.File), FileInfo}});
    TestEqual(TEXT("标题 Tooltip 仍为文件路径"), Groups[0]->Tooltip, Fixture.File);
    TestTrue(TEXT("分组附加原生监控汇总"), Groups[0]->AutoReimport.Label.Contains(TEXT("部分启用")));
    const auto Live = CaptureAutoReimportSnapshot();
    TestEqual(TEXT("生产设置快照不会修改总开关"), Live.bEnabled, GetDefault<UEditorLoadingSavingSettings>()->bMonitorContentDirectories);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantAutoReimportRefreshTest, "ImportAssistant.AutoReimport.SettingsRefresh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantAutoReimportRefreshTest::RunTest(const FString&)
{
    const bool bOriginalEnabled = GetDefault<UEditorLoadingSavingSettings>()->bMonitorContentDirectories;
    const TSharedRef<SImportAssistantWindow> Window = SNew(SImportAssistantWindow).LoadSavedSettings(false);
    TestTrue(TEXT("首次打开窗口留出监控配置观察期"), Window->AutoReimportSettingsSettleUntil > FPlatformTime::Seconds());
    Window->RefreshAutoReimport(true);
    TestTrue(TEXT("快照携带配置观察期，不依赖总开关是否开启"), Window->AutoReimportSnapshot.bSettingsSettling);
    TestFalse(TEXT("采集后清除配置脏标记"), Window->bAutoReimportSettingsDirty);
    Window->AutoReimportSettingsSettleUntil = 0;
    Window->RefreshAutoReimport(false);
    TestFalse(TEXT("观察期结束，即使目录相同也自动重新评估"), Window->AutoReimportSnapshot.bSettingsSettling);
    Window->OnAutoReimportSettingsChanged(GET_MEMBER_NAME_CHECKED(UEditorLoadingSavingSettings, AutoReimportDirectorySettings));
    TestTrue(TEXT("过滤规则变更触发观察期，即使监控根路径未变"), Window->bAutoReimportSettingsDirty &&
        Window->AutoReimportSettingsSettleUntil > FPlatformTime::Seconds());
    Window->RefreshAutoReimport(true);
    TestTrue(TEXT("强制刷新不能绕过过滤规则生效观察期"), Window->AutoReimportSnapshot.bSettingsSettling);
    Window->AutoReimportSettingsSettleUntil = 0;
    Window->OnAutoReimportSettingsChanged(GET_MEMBER_NAME_CHECKED(UEditorLoadingSavingSettings, bPromptBeforeAutoImporting));
    TestEqual(TEXT("只改确认策略无需重建目录监控"), Window->AutoReimportSettingsSettleUntil, 0.0);
    Window->RefreshAutoReimport(false);
    TestFalse(TEXT("确认策略变化仍更新快照"), Window->bAutoReimportSettingsDirty);
    TestEqual(TEXT("状态检查不修改生产监控设置"), GetDefault<UEditorLoadingSavingSettings>()->bMonitorContentDirectories, bOriginalEnabled);

    FMonitorFixture Fixture;
    const FString Root = TEXT("/Game/__ImportAssistantMonitor_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TStrongObjectPtr<UAnimSequence> Animation(NewObject<UAnimSequence>(CreatePackage(*(Root / TEXT("Probe"))), TEXT("Probe"), RF_Public | RF_Standalone));
    Animation->AssetImportData = NewObject<UAssetImportData>(Animation.Get());
    Animation->AssetImportData->SourceData.SourceFiles.Emplace(Fixture.File);
    FAssetRegistryModule::AssetCreated(Animation.Get());
    FRowPtr Row = MakeShared<FMatchRow>();
    Row->Asset = MakeRecord(FAssetData(Animation.Get()), TEXT(""));
    Row->File = Fixture.File;
    Window->Rows = {Row};
    Window->AutoReimportFiles.Add(NormalizePath(Fixture.File), EvaluateAutoReimportFile(Fixture.File, Fixture.Capture()));
    Window->UpdateAutoReimportRows();
    TestEqual(TEXT("窗口使用当前内存元数据确认来源关联"), Row->AutoReimport.State, EAutoReimportState::Enabled);
    Animation->AssetImportData->SourceData.SourceFiles[0].RelativeFilename = TEXT("C:/Changed/Other.fbx");
    Window->UpdateAutoReimportRows();
    TestEqual(TEXT("改绑来源后不再沿用原查询快照误报启用"), Row->AutoReimport.State, EAutoReimportState::Unrelated);
    TestEqual(TEXT("复核不覆盖报告中的原来源"), Row->Asset->Sources[0].Original, Fixture.File);
    FAssetRegistryModule::AssetDeleted(Animation.Get());
    Animation->ClearFlags(RF_Public | RF_Standalone);
    Animation->SetFlags(RF_Transient);
    Animation->GetPackage()->SetFlags(RF_Transient);
    Animation->GetPackage()->SetDirtyFlag(false);
    return true;
}
#endif
