#pragma once

#include "ImportAssistantStorageTemplates.h"

class UAssetImportData;
class USkeleton;

namespace ImportAssistant
{
FTopLevelAssetPath GetAssetClassPath(EImportAssistantAssetCategory Category);
TArray<FTopLevelAssetPath> GetSupportedAssetClasses();
bool IsSupportedFbxAsset(const UObject* Object);
FString GetAssetTypeLabel(const FTopLevelAssetPath& ClassPath);
UAssetImportData* GetImportData(UObject* Object);
USkeleton* GetRelatedSkeleton(UObject* Object);
TArray<UObject*> GetDirectReimportDependencies(UObject* Object);
FString DescribeSourceSlot(const FTopLevelAssetPath& ClassPath, int32 SourceIndex);
bool ValidateReimportSupport(UObject* Object, int32 SourceIndex, FString& Error);
bool HasValidImportedData(UObject* Object);
}
