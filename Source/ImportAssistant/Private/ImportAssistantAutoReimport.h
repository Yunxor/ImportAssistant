#pragma once

#include "FileCacheUtilities.h"
#include "ImportAssistantTypes.h"
#include "Settings/EditorLoadingSavingSettings.h"

namespace ImportAssistant
{
struct FAutoReimportDirectory
{
    FString Root;
    FString DisplayRoot;
    FString FilterDescription;
    DirectoryWatcher::FMatchRules Rules;
    bool bActive = false;
    bool bRulesKnown = false;
};

struct FAutoReimportSnapshot
{
    bool bEnabled = false;
    bool bPrompt = false;
    bool bDetectOnStartup = false;
    bool bSupportedFormatsKnown = false;
    bool bSettingsSettling = false;
    FString SettingsDescription;
    TArray<FAutoReimportDirectory> Directories;
};

TOptional<TArray<FString>> GetActiveAutoReimportDirectories();
FAutoReimportSnapshot BuildAutoReimportSnapshot(const UEditorLoadingSavingSettings& Settings,
    const TOptional<TArray<FString>>& ActiveDirectories, const FString& SupportedExtensions, bool bSettingsSettling = false);
FAutoReimportSnapshot CaptureAutoReimportSnapshot(bool bSettingsSettling = false);
FAutoReimportInfo EvaluateAutoReimportFile(const FString& File, const FAutoReimportSnapshot& Snapshot);
FAutoReimportInfo EvaluateAutoReimportAsset(const FAssetRecord& Asset, const FString& File, const FAutoReimportInfo& FileInfo);
FAutoReimportInfo SummarizeAutoReimport(const FAutoReimportInfo& FileInfo, const TArray<FRowPtr>& Rows, bool bPending);
FString BuildAutoReimportWarning(const TArray<FRowPtr>& Rows);
}
