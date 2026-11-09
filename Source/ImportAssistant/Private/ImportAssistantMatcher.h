#pragma once

#include "ImportAssistantTypes.h"
#include "AssetRegistry/AssetData.h"
#include <atomic>

namespace ImportAssistant
{
FString NormalizePath(FString Path);
FRecordPtr MakeRecord(const FAssetData& Data, const FString& DataSourceDirectory);
TArray<FString> MapPath(const FString& Path, const TArray<FPathMapping>& Mappings, TArray<int32>* OutAppliedRules = nullptr);
bool MatchesKeywords(const FString& AssetName, const FString& File, const FMatchSettings& Settings, TArray<FString>* OutMatchedKeywords = nullptr);
FString DescribeReason(EMatchReason Reason);
FString SourceFingerprint(const FAssetData& Data);
FSourceVersionInfo DescribeSourceVersion(const FAssetRecord& Asset, int32 SourceIndex);

struct FSearchSnapshot
{
    TArray<FRecordPtr> Records;
    TArray<FRecordPtr> UnknownRecords;
    TMap<FString, TArray<FRecordPtr>> BySource;
};

FSearchOutput Match(const FSearchSnapshot& Index, const TArray<FString>& Files, const FMatchSettings& Settings,
    const TArray<FPathMapping>& Mappings, const std::atomic_bool& Cancelled, const TArray<FFileSearchScope>& Scopes = {});
}




