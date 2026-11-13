#pragma once

#include "ImportAssistantTypes.h"
#include "HAL/CriticalSection.h"
#include "UObject/StrongObjectPtr.h"

class UAssetImportData;
class USkeleton;
class UPackage;

namespace ImportAssistant
{
FDirtyAssetInfo InspectDirtyAssets(const FAssetRecord& Asset);

// 仅观察计划涉及的包，从预检到确认、执行持续记录变更，不把已经脏的包当作不会继续变化。
class FPackageRevisionTracker
{
public:
    FPackageRevisionTracker();
    ~FPackageRevisionTracker();
    uint64 Watch(UPackage* Package);
    uint64 Get(UPackage* Package) const;

private:
    void Changed(UPackage* Package);
    mutable FCriticalSection Mutex;
    TMap<TWeakObjectPtr<UPackage>, uint64> Revisions;
    FDelegateHandle Marked, DirtyChanged, Modified, PropertyChanged, Transacted;
};

struct FPlannedUpdate
{
    FRowPtr Row;
    TStrongObjectPtr<UObject> Object;
    TWeakObjectPtr<UAssetImportData> ImportData;
    TArray<TWeakObjectPtr<UObject>> Dependencies;
    TArray<TWeakObjectPtr<UPackage>> Packages;
    FString ContentDescription;
    TWeakObjectPtr<USkeleton> Skeleton;
    FString SourceJson;
    FString OriginalSource;
    FString TargetFile;
    int32 SourceIndex = INDEX_NONE;
    bool bChangesSource = false;
    FDirtyAssetInfo Dirty;
};

struct FReimportPlan
{
    TSharedRef<FPackageRevisionTracker> Changes = MakeShared<FPackageRevisionTracker>();
    TMap<FSoftObjectPath, FPlannedUpdate> Items;
    TMap<TWeakObjectPtr<UPackage>, uint64> AcceptedRevisions;
    TMap<TWeakObjectPtr<UPackage>, bool> AcceptedDirtyStates;
    FString Error;
};

TSharedRef<FReimportPlan> BuildReimportPlan(const TArray<FRowPtr>& Rows, bool bUseCurrentSources = false);
FString BuildReimportWarning(const FReimportPlan& Plan);
FString DescribeReimportPlan(const FReimportPlan& Plan);
bool IsPlannedUpdateCurrent(const FReimportPlan& Plan, const FPlannedUpdate& Item, FString& Error);
bool IsReimportPlanCurrent(const FReimportPlan& Plan, FString& Error);
}
