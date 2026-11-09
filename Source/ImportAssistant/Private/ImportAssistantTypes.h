#pragma once

#include "CoreMinimal.h"

namespace ImportAssistant
{
// 后台任务只持有值快照，不访问设置 UObject。
struct FMatchSettings
{
    TArray<FString> IgnorePrefixes;
    TArray<FString> IgnoreSuffixes;
    bool bMatchPath = true;
    bool bMatchKeyword = false;
    bool bAllKeywords = true;
};

struct FPathMapping
{
    FString From;
    FString To;
};

struct FFileSearchScope
{
    FString File;
    TArray<FString> TemplateRoots;
    TArray<FString> SearchRoots;
    FString Summary;
    FString Details;
    bool bRecursive = true;
    bool bLimitedToTemplates = false;
};

struct FSourceRecord
{
    FString Original;
    TArray<FString> Candidates;
    FDateTime ImportedTimestampUtc = FDateTime(0);
};

struct FAssetRecord
{
    FSoftObjectPath ObjectPath;
    FString Name;
    FString PackageName;
    FTopLevelAssetPath AssetClassPath;
    FString SourceJson;
    FString Skeleton;
    TArray<FSourceRecord> Sources;
};

using FRecordPtr = TSharedPtr<const FAssetRecord, ESPMode::ThreadSafe>;

enum class EMatchReason : uint8
{
    None = 0,
    Exact = 1,
    Mapping = 2,
    Keyword = 4
};
ENUM_CLASS_FLAGS(EMatchReason);

enum class EAutoReimportState : uint8
{
    Enabled,
    Disabled,
    Excluded,
    Unrelated,
    Unknown
};

struct FAutoReimportInfo
{
    EAutoReimportState State = EAutoReimportState::Unknown;
    FString Label = TEXT("无法确认");
    FString Tooltip = TEXT("尚未评估原生自动重导入条件");
    bool bPrompt = false;
};

struct FDirtyAssetInfo
{
    FString Label = TEXT("无法确认");
    FString Tooltip = TEXT("尚未检查动画及关联 Skeleton 的未保存修改");
    FString AnimationPath;
    FString SkeletonPath;
    bool bAnimationDirty = false;
    bool bSkeletonDirty = false;
    bool bKnown = false;

    bool IsDirty() const { return bAnimationDirty || bSkeletonDirty; }
};

struct FSourceVersionInfo
{
    FString Label = TEXT("未知");
    FString Tooltip = TEXT("没有上次导入时记录的源文件版本时间");
};

struct FMatchRow
{
    FRecordPtr Asset;
    FString File;
    int32 SourceIndex = INDEX_NONE;
    EMatchReason Reason = EMatchReason::None;
    bool bSelected = false;
    bool bSourceAmbiguous = false;
    bool bFileConflict = false;
    FString Status;
    FString ReasonTooltip;
    bool bOutsideTemplate = false;
    FAutoReimportInfo AutoReimport;
    FDirtyAssetInfo Dirty;
    FSourceVersionInfo VersionTime;

    bool CanSelect() const { return !File.IsEmpty() && SourceIndex != INDEX_NONE && !bSourceAmbiguous; }
    FString Key() const { return File + TEXT("|") + Asset->ObjectPath.ToString(); }
};

using FRowPtr = TSharedPtr<FMatchRow>;

struct FSearchOutput
{
    TArray<FRowPtr> Rows;
    TArray<FString> UnmatchedFiles;
    int32 UnknownCount = 0;
    double Seconds = 0;
};
}







