#include "ImportAssistantAssetState.h"

#include "ImportAssistantAssetAccess.h"
#include "Engine/StaticMesh.h"
#include "Animation/Skeleton.h"
#include "AssetCompilingManager.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "ImportAssistantMatcher.h"
#include "InterchangeAssetImportData.h"
#include "InterchangeManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

namespace ImportAssistant
{
FDirtyAssetInfo InspectDirtyAssets(const FAssetRecord& Asset)
{
    FDirtyAssetInfo Info;
    Info.AssetPath = Asset.ObjectPath.ToString();
    Info.AssetLabel = GetAssetTypeLabel(Asset.AssetClassPath);
    UObject* Object = Asset.ObjectPath.ResolveObject();
    const bool bStatic = Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName() || (Object && Object->IsA<UStaticMesh>());
    USkeleton* Skeleton = GetRelatedSkeleton(Object);
    Info.SkeletonPath = bStatic ? FString() : Object ? (Skeleton ? Skeleton->GetPathName() : FString()) : FPackageName::ExportTextPathToObjectPath(Asset.Skeleton);
    if (Info.SkeletonPath == TEXT("None")) { Info.SkeletonPath.Reset(); }
    const FSoftObjectPath SkeletonPath(Info.SkeletonPath);
    Info.bKnown = bStatic || SkeletonPath.IsValid();
    const UPackage* Package = Object ? Object->GetPackage() : FindPackage(nullptr, *Asset.ObjectPath.GetLongPackageName());
    const UPackage* SkeletonPackage = Skeleton ? Skeleton->GetPackage() : (SkeletonPath.IsValid() ? FindPackage(nullptr, *SkeletonPath.GetLongPackageName()) : nullptr);
    Info.bAssetDirty = Package && Package->IsDirty();
    Info.bSkeletonDirty = SkeletonPackage && SkeletonPackage->IsDirty();
    if (Object)
    {
        for (UObject* Dependency : GetDirectReimportDependencies(Object))
        {
            if (Dependency != Skeleton && Dependency->GetPackage() != Package && Dependency->GetPackage()->IsDirty())
            {
                Info.DirtyDependencies.AddUnique(Dependency->GetPathName());
            }
        }
    }
    Info.Label = Info.IsDirty() ? TEXT("是：") : Info.bKnown ? TEXT("否") : TEXT("无法确认");
    TArray<FString> DirtyLabels;
    if (Info.bAssetDirty) { DirtyLabels.Add(Info.AssetLabel); }
    if (Info.bSkeletonDirty) { DirtyLabels.Add(TEXT("骨骼")); }
    if (!Info.DirtyDependencies.IsEmpty()) { DirtyLabels.Add(TEXT("关联依赖")); }
    Info.Label += FString::Join(DirtyLabels, TEXT("、"));
    Info.Tooltip = Info.IsDirty() ? TEXT("以下资产包存在未保存修改：") : TEXT("未发现未保存修改。");
    Info.Tooltip += TEXT("\n") + Info.AssetLabel + (Info.bAssetDirty ? TEXT("（未保存修改）：") : TEXT("（未发现修改）：")) + Info.AssetPath;
    if (!Info.SkeletonPath.IsEmpty())
    {
        Info.Tooltip += (Info.bSkeletonDirty ? TEXT("\nSkeleton（未保存修改）：") : TEXT("\nSkeleton（未发现修改）：")) + Info.SkeletonPath;
    }
    for (const FString& Dependency : Info.DirtyDependencies) { Info.Tooltip += TEXT("\n已脏的直接关联依赖：") + Dependency; }
    if (!Info.bKnown) { Info.Tooltip += TEXT("\n关联 Skeleton 尚无法确认。"); }
    return Info;
}

FPackageRevisionTracker::FPackageRevisionTracker()
{
    Marked = UPackage::PackageMarkedDirtyEvent.AddLambda([this](UPackage* Package, bool) { Changed(Package); });
    DirtyChanged = UPackage::PackageDirtyStateChangedEvent.AddRaw(this, &FPackageRevisionTracker::Changed);
    Modified = FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject* Object) { Changed(Object->GetPackage()); });
    PropertyChanged = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject* Object, FPropertyChangedEvent&)
        { Changed(Object->GetPackage()); });
    Transacted = FCoreUObjectDelegates::OnObjectTransacted.AddLambda([this](UObject* Object, const FTransactionObjectEvent&)
        { Changed(Object->GetPackage()); });
}

FPackageRevisionTracker::~FPackageRevisionTracker()
{
    UPackage::PackageMarkedDirtyEvent.Remove(Marked);
    UPackage::PackageDirtyStateChangedEvent.Remove(DirtyChanged);
    FCoreUObjectDelegates::OnObjectModified.Remove(Modified);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChanged);
    FCoreUObjectDelegates::OnObjectTransacted.Remove(Transacted);
}

void FPackageRevisionTracker::Changed(UPackage* Package)
{
    FScopeLock Lock(&Mutex);
    if (uint64* Revision = Revisions.Find(Package)) { ++*Revision; }
}

uint64 FPackageRevisionTracker::Watch(UPackage* Package)
{
    FScopeLock Lock(&Mutex);
    return Revisions.FindOrAdd(Package);
}

uint64 FPackageRevisionTracker::Get(UPackage* Package) const
{
    FScopeLock Lock(&Mutex);
    return Revisions.FindRef(Package);
}

TSharedRef<FReimportPlan> BuildReimportPlan(const TArray<FRowPtr>& Rows, bool bUseCurrentSources)
{
    const auto Plan = MakeShared<FReimportPlan>();
    TArray<UObject*> Compiling;
    for (const FRowPtr& Row : Rows)
    {
        if (!Row->bSelected || !Row->CanSelect() || Plan->Items.Contains(Row->Asset->ObjectPath)) { continue; }
        FPlannedUpdate& Item = Plan->Items.Add(Row->Asset->ObjectPath);
        Item.Row = Row;
        Item.SourceIndex = Row->SourceIndex;
        Item.TargetFile = Row->File;
        Item.Object.Reset(Row->Asset->ObjectPath.TryLoad());
        if (Item.Object.IsValid()) { Compiling.Add(Item.Object.Get()); }
    }
    FAssetCompilingManager::Get().FinishCompilationForObjects(Compiling);
    for (auto& Pair : Plan->Items)
    {
        FPlannedUpdate& Item = Pair.Value;
        UObject* Object = Item.Object.Get();
        FString Reason;
        if (!ValidateReimportSupport(Object, Item.SourceIndex, Reason))
        {
            Plan->Error += Reason + TEXT("：") + Pair.Key.ToString() + TEXT("\n");
            continue;
        }
        Item.ImportData = GetImportData(Object);
        Item.SourceJson = SourceFingerprint(FAssetData(Object));
        // 报告模式允许再次执行：以当前真实来源形成新计划；普通检索模式仍拒绝失效的来源快照。
        if (!bUseCurrentSources && Item.SourceJson != Item.Row->Asset->SourceJson)
        {
            Plan->Error += TEXT("来源元数据已变化，请重新检索：") + Pair.Key.ToString() + TEXT("\n");
            continue;
        }
        TUniquePtr<UE::Interchange::FScopedInterchangeImportEnableState> LegacyMode;
        if (!Item.ImportData->IsA<UInterchangeAssetImportData>())
        {
            LegacyMode = MakeUnique<UE::Interchange::FScopedInterchangeImportEnableState>(false);
        }
        TArray<FString> Sources;
        if (!FReimportManager::Instance()->CanReimport(Object, &Sources) || !Sources.IsValidIndex(Item.SourceIndex))
        {
            Plan->Error += TEXT("无法确认实际重导入来源槽位：") + Pair.Key.ToString() + TEXT("\n");
            continue;
        }
        Item.OriginalSource = Sources[Item.SourceIndex];
        Item.bChangesSource = NormalizePath(Item.OriginalSource) != NormalizePath(Item.TargetFile);
        Item.Skeleton = GetRelatedSkeleton(Object);
        Item.ContentDescription = Item.ImportData->IsA<UInterchangeAssetImportData>() ? FString::Printf(TEXT("Interchange 来源槽位 %d"), Item.SourceIndex) :
            DescribeSourceSlot(Object->GetClass()->GetClassPathName(), Item.SourceIndex);
        Item.Dependencies.Reset();
        Item.Packages.AddUnique(Object->GetPackage());
        for (UObject* Dependency : GetDirectReimportDependencies(Object))
        {
            Item.Dependencies.Add(Dependency);
            Item.Packages.AddUnique(Dependency->GetPackage());
        }
        Item.Dirty = InspectDirtyAssets(*Item.Row->Asset);
        Item.Row->Dirty = Item.Dirty;
    }
    // 所有所选对象加载并完成编译后才定稿，避免后加载的共享骨骼修改遗漏在确认范围之外。
    for (auto& Pair : Plan->Items)
    {
        FPlannedUpdate& Item = Pair.Value;
        if (!Item.Object.IsValid() || !Item.ImportData.IsValid()) { continue; }
        Item.Dirty = InspectDirtyAssets(*Item.Row->Asset);
        Item.Row->Dirty = Item.Dirty;
        for (const auto& WeakPackage : Item.Packages)
        {
            UPackage* Package = WeakPackage.Get();
            Plan->AcceptedRevisions.Add(Package, Plan->Changes->Watch(Package));
            Plan->AcceptedDirtyStates.Add(Package, Package->IsDirty());
        }
    }
    return Plan;
}

FString DescribeReimportPlan(const FReimportPlan& Plan)
{
    TArray<FString> Lines;
    for (const auto& Pair : Plan.Items)
    {
        const FPlannedUpdate& Item = Pair.Value;
        Lines.Add(Pair.Key.ToString() + TEXT("\n原来源：") + Item.OriginalSource + TEXT("\n本次来源：") + Item.TargetFile +
            TEXT("\n更新内容：") + Item.ContentDescription + TEXT("\n是否脏数据：") + Item.Dirty.Label + TEXT("\n") + Item.Dirty.Tooltip);
    }
    Lines.Sort();
    return FString::Join(Lines, TEXT("\n\n"));
}

FString BuildReimportWarning(const FReimportPlan& Plan)
{
    TArray<FString> Sources;
    TSet<FString> DirtyAssets;
    for (const auto& Pair : Plan.Items)
    {
        const FPlannedUpdate& Item = Pair.Value;
        if (Item.bChangesSource || Item.SourceIndex > 0)
        {
            Sources.Add(Pair.Key.ToString() + TEXT("\n更新内容：") + Item.ContentDescription + TEXT("\n原来源：") + Item.OriginalSource + TEXT("\n本次来源：") + Item.TargetFile);
        }
        if (Item.Dirty.bAssetDirty) { DirtyAssets.Add(Item.Dirty.AssetLabel + TEXT("：") + Item.Dirty.AssetPath); }
        if (Item.Dirty.bSkeletonDirty) { DirtyAssets.Add(TEXT("Skeleton：") + Item.Dirty.SkeletonPath); }
        for (const FString& Dependency : Item.Dirty.DirtyDependencies) { DirtyAssets.Add(TEXT("关联依赖：") + Dependency); }
    }
    if (Sources.IsEmpty() && DirtyAssets.IsEmpty()) { return {}; }
    Sources.Sort();
    TArray<FString> Dirty = DirtyAssets.Array();
    Dirty.Sort();
    FString Message = FString::Printf(TEXT("更新 %d 个资产，仅修改内存，不自动保存或签出。完整计划已复制到剪贴板。\n"), Plan.Items.Num());
    if (!Sources.IsEmpty())
    {
        Message += FString::Printf(TEXT("\n来源改绑或分项内容更新（%d 项）：\n%s%s\n"), Sources.Num(),
            *FString::Join(TArray<FString>(Sources.GetData(), FMath::Min(Sources.Num(), 5)), TEXT("\n\n")), Sources.Num() > 5 ? TEXT("\n其余见剪贴板") : TEXT(""));
    }
    if (!Dirty.IsEmpty())
    {
        Message += FString::Printf(TEXT("\n将覆盖已有未保存修改的资产（%d 个，已去重）：\n%s%s\n"), Dirty.Num(),
            *FString::Join(TArray<FString>(Dirty.GetData(), FMath::Min(Dirty.Num(), 10)), TEXT("\n")), Dirty.Num() > 10 ? TEXT("\n其余见剪贴板") : TEXT(""));
    }
    return Message + TEXT("\n将按原生重导入写入数据，可能覆盖已有编辑内容，不再因已确认的脏状态而跳过；不是先清空整个 Skeleton。") +
        TEXT("资产、Skeleton 及其他依赖由你检查后手动保存，不保证可以一次撤销。请勿并行编辑相关资产或运行其他导入任务。继续？");
}

bool IsPlannedUpdateCurrent(const FReimportPlan& Plan, const FPlannedUpdate& Item, FString& Error)
{
    UObject* Object = Item.Object.Get();
    if (!Object || GetRelatedSkeleton(Object) != Item.Skeleton.Get() || GetImportData(Object) != Item.ImportData.Get() || !Item.ImportData.IsValid() ||
        SourceFingerprint(FAssetData(Object)) != Item.SourceJson || Item.Row->SourceIndex != Item.SourceIndex ||
        NormalizePath(Item.Row->File) != NormalizePath(Item.TargetFile))
    {
        Error = TEXT("资产、导入数据、Skeleton 或来源信息已变化，请重新检索并确认");
        return false;
    }
    const auto Dependencies = GetDirectReimportDependencies(Object);
    if (Dependencies.Num() != Item.Dependencies.Num() || Item.Dependencies.ContainsByPredicate([&Dependencies](const auto& Dependency)
        { return !Dependencies.Contains(Dependency.Get()); }))
    {
        Error = TEXT("直接关联依赖已变化，请重新确认"); return false;
    }
    for (const auto& WeakPackage : Item.Packages)
    {
        UPackage* Package = WeakPackage.Get();
        const uint64* Accepted = Plan.AcceptedRevisions.Find(Package);
        if (!Package || !Accepted || *Accepted != Plan.Changes->Get(Package) || Package->IsDirty() != Plan.AcceptedDirtyStates.FindRef(Package))
        {
            Error = TEXT("出现确认范围之外的新修改，请重新确认：") + (Package ? Package->GetName() : TEXT("依赖包已失效"));
            return false;
        }
    }
    const auto Sources = Item.ImportData->ExtractFilenames();
    if (!Sources.IsValidIndex(Item.SourceIndex) || NormalizePath(Sources[Item.SourceIndex]) != NormalizePath(Item.OriginalSource))
    {
        Error = TEXT("实际来源解析已变化，请重新检索并确认");
        return false;
    }
    return true;
}

bool IsReimportPlanCurrent(const FReimportPlan& Plan, FString& Error)
{
    if (!Plan.Error.IsEmpty()) { Error = Plan.Error; return false; }
    for (const auto& Pair : Plan.Items) { if (!IsPlannedUpdateCurrent(Plan, Pair.Value, Error)) { return false; } }
    return true;
}
}
