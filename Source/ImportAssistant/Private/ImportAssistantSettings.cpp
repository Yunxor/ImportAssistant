#include "ImportAssistantSettings.h"

#include "HAL/FileManager.h"
#include "ImportAssistantMatcher.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

UImportAssistantSettings::UImportAssistantSettings()
{
    AssetDirectories.AddDefaulted_GetRef().Path = TEXT("/Game");
}

FString UImportAssistantSettings::GetPluginConfigFilename() const
{
    return FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin(TEXT("ImportAssistant"))->GetBaseDir() / TEXT("Config/ImportAssistant.ini"));
}

bool UImportAssistantSettings::LoadPluginConfig(FString& OutError)
{
    const FString Filename = GetPluginConfigFilename();
    if (IFileManager::Get().FileExists(*Filename)) { return LoadFromFile(Filename, OutError); }
    MigrateLegacyConfig(GEditorPerProjectIni);
    return SaveToFile(Filename, OutError);
}

bool UImportAssistantSettings::SavePluginConfig(FString& OutError)
{
    return SaveToFile(GetPluginConfigFilename(), OutError);
}

bool UImportAssistantSettings::LoadFromFile(const FString& Filename, FString& OutError)
{
    FConfigFile File;
    File.Read(Filename);
    if (!File.FindSection(GetClass()->GetPathName()))
    {
        OutError = TEXT("无法读取插件配置节：") + Filename;
        return false;
    }
    // 完整配置中的空数组不会输出条目，先清空，避免重新继承构造函数中的 /Game 默认项。
    AssetDirectories.Reset();
    IgnorePrefixes.Reset();
    IgnoreSuffixes.Reset();
    StorageTemplates.Reset();
    HiddenResultColumns.Reset();
    bUseStorageTemplates = false;
    StorageSearchMode = EImportAssistantStorageSearchMode::PreferTemplate;
    FLoadConfigParams Params;
    Params.ConfigClass = GetClass();
    Params.OverrideFile = &File;
    LoadConfig(Params);
    OutError.Reset();
    return true;
}

bool UImportAssistantSettings::SaveToFile(const FString& Filename, FString& OutError)
{
    const FString FullPath = FPaths::ConvertRelativePathToFull(Filename);
    if (IFileManager::Get().IsReadOnly(*FullPath))
    {
        OutError = TEXT("插件配置只读，请先手动签出或调整写入权限：") + FullPath;
        return false;
    }
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullPath), true))
    {
        OutError = TEXT("无法创建配置目录：") + FPaths::GetPath(FullPath);
        return false;
    }
    // 由 UE 反射序列化全部 Config 属性；独立缓存避免污染用户配置和类默认对象，Write 返回真实写入结果。
    FConfigFile Existing;
    if (IFileManager::Get().FileExists(*FullPath)) { Existing.Read(FullPath); }
    FConfigCacheIni Cache(EConfigCacheType::Temporary);
    Cache.SetFile(FullPath, &Existing);
    SaveConfig(CPF_Config, *FullPath, &Cache, false);
    FConfigFile* File = Cache.FindConfigFile(FullPath);
    File->Dirty = true;
    File->bCanSaveAllSections = true;
    if (!File->Write(FullPath))
    {
        OutError = TEXT("插件配置保存失败，当前改动仅在内存中：") + FullPath;
        return false;
    }
    OutError.Reset();
    return true;
}

void UImportAssistantSettings::MigrateLegacyConfig(const FString& Filename)
{
    const TCHAR* Section = TEXT("ImportAssistant");
    FString RootsText, PrefixesText, SuffixesText;
    if (GConfig->GetString(Section, TEXT("Roots"), RootsText, Filename))
    {
        TArray<FString> Roots;
        RootsText.ParseIntoArray(Roots, TEXT(";"), true);
        AssetDirectories.Reset();
        for (FString Root : Roots)
        {
            Root.TrimStartAndEndInline();
            if (!Root.IsEmpty()) { AssetDirectories.AddDefaulted_GetRef().Path = MoveTemp(Root); }
        }
    }
    if (GConfig->GetString(Section, TEXT("IgnorePrefixes"), PrefixesText, Filename))
    {
        IgnorePrefixes.Reset();
        PrefixesText.ParseIntoArray(IgnorePrefixes, TEXT(";"), true);
        for (FString& Prefix : IgnorePrefixes) { Prefix.TrimStartAndEndInline(); }
    }
    if (GConfig->GetString(Section, TEXT("IgnoreSuffixes"), SuffixesText, Filename))
    {
        IgnoreSuffixes.Reset();
        SuffixesText.ParseIntoArray(IgnoreSuffixes, TEXT(";"), true);
        for (FString& Suffix : IgnoreSuffixes) { Suffix.TrimStartAndEndInline(); }
    }
    GConfig->GetString(Section, TEXT("Mappings"), MappingsText, Filename);
    GConfig->GetBool(Section, TEXT("Recursive"), bRecursive, Filename);
    GConfig->GetBool(Section, TEXT("MatchPath"), bMatchPath, Filename);
    GConfig->GetBool(Section, TEXT("MatchKeyword"), bMatchKeyword, Filename);
    GConfig->GetBool(Section, TEXT("AllKeywords"), bAllKeywords, Filename);
}

bool UImportAssistantSettings::Parse(TArray<FName>& OutRoots, TArray<ImportAssistant::FPathMapping>& OutMappings, FString& OutError) const
{
    OutRoots.Reset();
    OutMappings.Reset();
    OutError.Reset();
    for (const FDirectoryPath& Directory : AssetDirectories)
    {
        FString Root = Directory.Path.TrimStartAndEnd();
        Root.ReplaceInline(TEXT("\\"), TEXT("/"));
        Root.RemoveFromEnd(TEXT("/"));
        FString DiskPath;
        if (Root.IsEmpty() || !FPackageName::IsValidLongPackageName(Root / TEXT("Probe")) ||
            !FPackageName::TryConvertLongPackageNameToFilename(Root / TEXT("Probe"), DiskPath))
        {
            OutError = FString::Printf(TEXT("资产目录无效或尚未挂载：%s"), *Root);
            return false;
        }
        OutRoots.AddUnique(FName(Root));
    }
    if (OutRoots.IsEmpty() || (!bMatchPath && !bMatchKeyword))
    {
        OutError = TEXT("请指定至少一个已挂载资产目录，并启用至少一种匹配方式。");
        return false;
    }
    TArray<FString> Lines;
    MappingsText.ParseIntoArrayLines(Lines, true);
    for (FString Line : Lines)
    {
        Line.TrimStartAndEndInline();
        if (Line.IsEmpty()) { continue; }
        FString From, To;
        if (!Line.Split(TEXT("=>"), &From, &To))
        {
            OutError = TEXT("每条路径映射使用 旧目录 => 新目录，且每行一条。");
            return false;
        }
        From.TrimStartAndEndInline();
        To.TrimStartAndEndInline();
        if (From.IsEmpty() || To.IsEmpty() || FPaths::IsRelative(From) || FPaths::IsRelative(To))
        {
            OutError = TEXT("路径映射两端必须为非空的绝对目录。");
            return false;
        }
        OutMappings.Add({ ImportAssistant::NormalizePath(From), ImportAssistant::NormalizePath(To) });
    }
    return true;
}

ImportAssistant::FMatchSettings UImportAssistantSettings::MakeMatchSettings() const
{
    return { IgnorePrefixes, IgnoreSuffixes, bMatchPath, bMatchKeyword, bAllKeywords };
}


