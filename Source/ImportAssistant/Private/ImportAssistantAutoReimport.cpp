#include "ImportAssistantAutoReimport.h"

#include "AutoReimport/AutoReimportManager.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Factories/Factory.h"
#include "ImportAssistantMatcher.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/WildcardString.h"
#include "UObject/UObjectIterator.h"

namespace ImportAssistant
{
namespace
{
FString DirectoryKey(const FString& Directory)
{
    FString Key = NormalizePath(Directory);
    if (!Key.EndsWith(TEXT("/"))) { Key += TEXT("/"); }
    return Key;
}
}

TOptional<TArray<FString>> GetActiveAutoReimportDirectories()
{
    const UUnrealEdEngine* Engine = Cast<UUnrealEdEngine>(GEditor);
    if (!Engine || !Engine->AutoReimportManager) { return {}; }
    TArray<FString> Roots;
    for (const FPathAndMountPoint& Directory : Engine->AutoReimportManager->GetMonitoredDirectories())
    {
        Roots.AddUnique(DirectoryKey(Directory.Path));
    }
    Roots.Sort();
    return Roots;
}

FAutoReimportSnapshot BuildAutoReimportSnapshot(const UEditorLoadingSavingSettings& Settings,
    const TOptional<TArray<FString>>& ActiveDirectories, const FString& SupportedExtensions, bool bSettingsSettling)
{
    FAutoReimportSnapshot Snapshot;
    Snapshot.bEnabled = Settings.bMonitorContentDirectories;
    Snapshot.bPrompt = Settings.bPromptBeforeAutoImporting;
    Snapshot.bDetectOnStartup = Settings.bDetectChangesOnStartup;
    Snapshot.bSupportedFormatsKnown = !SupportedExtensions.IsEmpty();
    Snapshot.bSettingsSettling = bSettingsSettling;
    Snapshot.SettingsDescription = FString::Printf(TEXT("Monitor Content Directories：%s\n处理前确认：%s\n启动时检测变更：%s"),
        Snapshot.bEnabled ? TEXT("开启") : TEXT("关闭"), Snapshot.bPrompt ? TEXT("需要确认") : TEXT("无需确认"),
        Snapshot.bDetectOnStartup ? TEXT("开启") : TEXT("关闭"));
    if (!Snapshot.bEnabled) { return Snapshot; }

    const FAutoReimportDirectoryConfig::FParseContext Context(false);
    TArray<FAutoReimportDirectory> Parsed;
    for (const FAutoReimportDirectoryConfig& Config : Settings.AutoReimportDirectorySettings)
    {
        FString Root = Config.SourceDirectory;
        FString MountPoint = Config.MountPoint;
        if (!FAutoReimportDirectoryConfig::ParseSourceDirectoryAndMountPoint(Root, MountPoint, Context)) { continue; }
        FAutoReimportDirectory& Directory = Parsed.AddDefaulted_GetRef();
        Directory.Root = DirectoryKey(Root);
        Directory.DisplayRoot = Config.SourceDirectory + TEXT(" → ") + FPaths::ConvertRelativePathToFull(Root);
        Directory.bRulesKnown = true;
        if (Snapshot.bSupportedFormatsKnown) { Directory.Rules.SetApplicableExtensions(SupportedExtensions); }
        TArray<FString> Filters;
        for (const FAutoReimportWildcard& Wildcard : Config.Wildcards)
        {
            Directory.Rules.AddWildcardRule(Wildcard.Wildcard, Wildcard.bInclude);
            Filters.Add((Wildcard.bInclude ? TEXT("包含：") : TEXT("排除：")) + Wildcard.Wildcard);
        }
        Directory.FilterDescription = Filters.IsEmpty() ? TEXT("目录通配符：未设置") : FString::Join(Filters, TEXT("\n"));
    }
    // 与原生设置顺序一致：被后续父目录覆盖的前项不会成为有效监控。
    for (int32 Index = 0; Index < Parsed.Num(); ++Index)
    {
        bool bSuppressed = false;
        for (int32 Other = Index + 1; Other < Parsed.Num(); ++Other)
        {
            if (Parsed[Index].Root.StartsWith(Parsed[Other].Root)) { bSuppressed = true; break; }
        }
        if (bSuppressed) { continue; }
        FAutoReimportDirectory Directory = Parsed[Index];
        if (ActiveDirectories.IsSet())
        {
            Directory.bActive = ActiveDirectories->ContainsByPredicate([&Directory](const FString& Active)
                { return DirectoryKey(Active) == Directory.Root; });
        }
        Snapshot.Directories.Add(MoveTemp(Directory));
    }
    if (ActiveDirectories.IsSet())
    {
        for (const FString& Root : *ActiveDirectories)
        {
            const FString Key = DirectoryKey(Root);
            if (Snapshot.Directories.ContainsByPredicate([&Key](const FAutoReimportDirectory& Directory) { return Directory.Root == Key; })) { continue; }
            // 原生公开接口只返回运行时目录，拿不到第三方添加的过滤规则，因此只能报告无法确认。
            FAutoReimportDirectory& Directory = Snapshot.Directories.AddDefaulted_GetRef();
            Directory.Root = Key;
            Directory.DisplayRoot = Root;
            Directory.bActive = true;
            Directory.FilterDescription = TEXT("此监控不在当前目录配置中，无法读取其运行时过滤规则");
        }
    }
    return Snapshot;
}

FAutoReimportSnapshot CaptureAutoReimportSnapshot(bool bSettingsSettling)
{
    check(IsInGameThread());
    const UEditorLoadingSavingSettings& Settings = *GetDefault<UEditorLoadingSavingSettings>();
    if (!Settings.bMonitorContentDirectories) { return BuildAutoReimportSnapshot(Settings, {}, {}, bSettingsSettling); }
    FString Extensions;
    TSet<FString> Seen;
    // 与原生 Auto Reimport 一样，从已注册的编辑器导入工厂收集扩展名，不加载待检索资产。
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (!It->IsChildOf(UFactory::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract)) { continue; }
        const UFactory* Factory = CastChecked<UFactory>(It->GetDefaultObject());
        if (!Factory->bEditorImport) { continue; }
        for (const FString& Format : Factory->Formats)
        {
            FString Extension, Description;
            if (Format.Split(TEXT(";"), &Extension, &Description) && !Extension.IsEmpty() && !Seen.Contains(Extension))
            {
                Extensions += Extension + TEXT(";");
                Seen.Add(Extension);
            }
        }
    }
    return BuildAutoReimportSnapshot(Settings, GetActiveAutoReimportDirectories(), Extensions, bSettingsSettling);
}

FAutoReimportInfo EvaluateAutoReimportFile(const FString& File, const FAutoReimportSnapshot& Snapshot)
{
    if (!Snapshot.bEnabled) { return {EAutoReimportState::Disabled, TEXT("未启用"), TEXT("原生目录监控总开关关闭。\n") + Snapshot.SettingsDescription}; }
    if (File.IsEmpty()) { return {EAutoReimportState::Unknown, TEXT("无法确认"), TEXT("没有当前 FBX 路径，无法确认监控范围。\n") + Snapshot.SettingsDescription}; }
    FString FullFilename = FPaths::ConvertRelativePathToFull(File);
    FPaths::NormalizeFilename(FullFilename);
    FPaths::CollapseRelativeDirectories(FullFilename);
    const FString FullPath = NormalizePath(FullFilename);
    TArray<FString> Reasons;
    bool bExcluded = false;
    bool bUncertain = false;
    for (const FAutoReimportDirectory& Directory : Snapshot.Directories)
    {
        if (!FullPath.StartsWith(Directory.Root)) { continue; }
        const FString Relative = FullFilename.RightChop(Directory.Root.Len());
        const FString Context = TEXT("FBX：") + File + TEXT("\n监控目录：") + Directory.DisplayRoot + TEXT("\n相对路径：") + Relative +
            TEXT("\n") + Directory.FilterDescription;
        if (!Directory.bActive || !Directory.bRulesKnown || !Snapshot.bSupportedFormatsKnown || Snapshot.bSettingsSettling)
        {
            bUncertain = true;
            Reasons.Add(Context + (Snapshot.bSettingsSettling ? TEXT("\n监控配置刚变更或窗口刚打开，等待原生监控刷新后重新评估。") :
                TEXT("\n无法确认：目录尚未生效、运行时过滤规则不可见，或尚未获得支持的导入格式。")));
            continue;
        }
        if (Directory.Rules.IsFileApplicable(*Relative))
        {
            return {EAutoReimportState::Enabled, TEXT("FBX 已受监控"), Context + TEXT("\n文件通过 UE 原生格式和通配符过滤。\n") + Snapshot.SettingsDescription, Snapshot.bPrompt};
        }
        bExcluded = true;
        Reasons.Add(Context + TEXT("\n文件未通过包含/排除通配符或编辑器导入工厂的扩展名过滤。"));
    }
    if (bUncertain) { return {EAutoReimportState::Unknown, TEXT("无法确认"), FString::Join(Reasons, TEXT("\n\n")) + TEXT("\n") + Snapshot.SettingsDescription}; }
    if (bExcluded) { return {EAutoReimportState::Excluded, TEXT("被规则排除"), FString::Join(Reasons, TEXT("\n\n")) + TEXT("\n") + Snapshot.SettingsDescription}; }
    return {EAutoReimportState::Disabled, TEXT("未启用"), TEXT("FBX 不在已配置或实际生效的监控目录内：") + File + TEXT("\n") + Snapshot.SettingsDescription};
}

FAutoReimportInfo EvaluateAutoReimportAsset(const FAssetRecord& Asset, const FString& File, const FAutoReimportInfo& FileInfo)
{
    if (FileInfo.State != EAutoReimportState::Enabled) { return FileInfo; }
    FString PackageFilename;
    const bool bPackageResolved = FPackageName::TryConvertLongPackageNameToFilename(Asset.PackageName, PackageFilename);
    const FString FileKey = NormalizePath(File);
    bool bHasSource = false;
    for (const FSourceRecord& Source : Asset.Sources)
    {
        if (Source.Original.IsEmpty()) { continue; }
        bHasSource = true;
        // 与原生源文件缓存一致：仅包目录相对路径和引擎基准相对路径；不使用插件映射或关键词候选。
        const FString BaseCandidate = NormalizePath(Source.Original);
        const FString PackageCandidate = bPackageResolved ? NormalizePath(FPaths::GetPath(PackageFilename) / Source.Original) : FString();
        if (BaseCandidate.Equals(FileKey) || PackageCandidate.Equals(FileKey))
        {
            return {EAutoReimportState::Enabled, FileInfo.bPrompt ? TEXT("已启用（需确认）") : TEXT("已启用"),
                TEXT("资产来源关联当前受监控 FBX。\n资产：") + Asset.ObjectPath.ToString() + TEXT("\n来源记录：") + Source.Original +
                TEXT("\n") + FileInfo.Tooltip, FileInfo.bPrompt};
        }
    }
    if (!bHasSource || !bPackageResolved)
    {
        return {EAutoReimportState::Unknown, TEXT("无法确认"), TEXT("FBX 受监控，但资产缺少可确认的来源记录或有效包目录。\n") + FileInfo.Tooltip};
    }
    TArray<FString> Sources;
    for (const FSourceRecord& Source : Asset.Sources) { if (!Source.Original.IsEmpty()) { Sources.Add(Source.Original); } }
    return {EAutoReimportState::Unrelated, TEXT("来源不关联"),
        TEXT("FBX 受监控，但资产记录的来源不是当前文件。\n资产来源记录：\n") +
        FString::Join(Sources, TEXT("\n")) + TEXT("\n") + FileInfo.Tooltip};
}

FAutoReimportInfo SummarizeAutoReimport(const FAutoReimportInfo& FileInfo, const TArray<FRowPtr>& Rows, bool bPending)
{
    if (FileInfo.State != EAutoReimportState::Enabled) { return FileInfo; }
    if (bPending) { return {EAutoReimportState::Unknown, TEXT("FBX 已受监控；资产关联待检索完成确认"), FileInfo.Tooltip}; }
    int32 Enabled = 0;
    int32 Unknown = 0;
    for (const FRowPtr& Row : Rows)
    {
        Enabled += Row->AutoReimport.State == EAutoReimportState::Enabled ? 1 : 0;
        Unknown += Row->AutoReimport.State == EAutoReimportState::Unknown ? 1 : 0;
    }
    const FString Counts = FString::Printf(TEXT("%d / %d 个匹配资产关联当前受监控 FBX"), Enabled, Rows.Num());
    if (Enabled > 0)
    {
        return {EAutoReimportState::Enabled, (Enabled == Rows.Num() ? TEXT("已启用（") : TEXT("部分启用（")) + Counts + TEXT("）"), FileInfo.Tooltip};
    }
    return {Unknown > 0 ? EAutoReimportState::Unknown : EAutoReimportState::Unrelated,
        Rows.IsEmpty() ? TEXT("FBX 已受监控；本范围未找到匹配资产") : (Unknown > 0 ? TEXT("FBX 已受监控；资产关联无法确认") : TEXT("FBX 已受监控；匹配资产来源均不关联")),
        FileInfo.Tooltip};
}

FString BuildAutoReimportWarning(const TArray<FRowPtr>& Rows)
{
    TMap<FString, int32> Affected;
    for (const FRowPtr& Row : Rows)
    {
        if (Row->bSelected && Row->CanSelect() && Row->AutoReimport.State == EAutoReimportState::Enabled) { ++Affected.FindOrAdd(Row->File); }
    }
    if (Affected.IsEmpty()) { return {}; }
    TArray<FString> Files;
    Affected.GenerateKeyArray(Files);
    Files.Sort();
    TArray<FString> Lines;
    for (const FString& File : Files) { Lines.Add(FString::Printf(TEXT("%s：%d 个所选资产"), *File, Affected[File])); }
    return TEXT("以下所选资产的来源关联受监控 FBX，原生自动重导入可能与本次操作重复执行，且有独立的保存流程：\n\n") +
        FString::Join(Lines, TEXT("\n")) + TEXT("\n\n可关闭对应源目录监控或等待原生任务完成后再操作。此提示不表示当前一定有待处理任务。仍继续？");
}
}


