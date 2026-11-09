#include "ImportAssistantAssetState.h"

#include "Animation/AnimSequence.h"
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
    Info.AnimationPath = Asset.ObjectPath.ToString();
    UAnimSequence* Animation = Cast<UAnimSequence>(Asset.ObjectPath.ResolveObject());
    Info.SkeletonPath = Animation ? (Animation->GetSkeleton() ? Animation->GetSkeleton()->GetPathName() : FString()) :
        FPackageName::ExportTextPathToObjectPath(Asset.Skeleton);
    if (Info.SkeletonPath == TEXT("None")) { Info.SkeletonPath.Reset(); }
    const FSoftObjectPath SkeletonPath(Info.SkeletonPath);
    Info.bKnown = SkeletonPath.IsValid();
    const UPackage* AnimationPackage = Animation ? Animation->GetPackage() : FindPackage(nullptr, *Asset.ObjectPath.GetLongPackageName());
    const UPackage* SkeletonPackage = Animation && Animation->GetSkeleton() ? Animation->GetSkeleton()->GetPackage() :
        (Info.bKnown ? FindPackage(nullptr, *SkeletonPath.GetLongPackageName()) : nullptr);
    Info.bAnimationDirty = AnimationPackage && AnimationPackage->IsDirty();
    Info.bSkeletonDirty = SkeletonPackage && SkeletonPackage->IsDirty();
    if (Info.IsDirty())
    {
        Info.Label = Info.bAnimationDirty ? (Info.bSkeletonDirty ? TEXT("是：动画、骨骼") : TEXT("是：动画")) : TEXT("是：骨骼");
        Info.Tooltip = TEXT("以下资产包存在未保存修改：");
        if (Info.bAnimationDirty) { Info.Tooltip += TEXT("\n动画：") + Info.AnimationPath; }
        if (Info.bSkeletonDirty) { Info.Tooltip += TEXT("\nSkeleton：") + Info.SkeletonPath; }
    }
    else
    {
        Info.Label = Info.bKnown ? TEXT("否") : TEXT("无法确认");
        Info.Tooltip = Info.bKnown ? TEXT("动画和关联 Skeleton 均无未保存修改。\n动画：") + Info.AnimationPath +
            TEXT("\nSkeleton：") + Info.SkeletonPath : TEXT("动画未发现未保存修改，但缺少可确认的关联 Skeleton 信息。\n动画：") + Info.AnimationPath;
    }
    if (!Info.bKnown && Info.IsDirty()) { Info.Tooltip += TEXT("\n关联 Skeleton 尚无法确认。"); }
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
        Item.Animation.Reset(Cast<UAnimSequence>(Row->Asset->ObjectPath.TryLoad()));
        if (Item.Animation.IsValid()) { Compiling.Add(Item.Animation.Get()); }
    }
    FAssetCompilingManager::Get().FinishCompilationForObjects(Compiling);
    for (auto& Pair : Plan->Items)
    {
        FPlannedUpdate& Item = Pair.Value;
        UAnimSequence* Animation = Item.Animation.Get();
        if (!Animation || !Animation->AssetImportData || !Animation->GetSkeleton())
        {
            Plan->Error += TEXT("缺少动画、导入数据或 Skeleton：") + Pair.Key.ToString() + TEXT("\n");
            continue;
        }
        Item.SourceJson = SourceFingerprint(FAssetData(Animation));
        // 报告模式允许再次执行：以当前真实来源形成新计划；普通检索模式仍拒绝失效的来源快照。
        if (!bUseCurrentSources && Item.SourceJson != Item.Row->Asset->SourceJson)
        {
            Plan->Error += TEXT("来源元数据已变化，请重新检索：") + Pair.Key.ToString() + TEXT("\n");
            continue;
        }
        TUniquePtr<UE::Interchange::FScopedInterchangeImportEnableState> LegacyMode;
        if (!Animation->AssetImportData->IsA<UInterchangeAssetImportData>())
        {
            LegacyMode = MakeUnique<UE::Interchange::FScopedInterchangeImportEnableState>(false);
        }
        TArray<FString> Sources;
        if (!FReimportManager::Instance()->CanReimport(Animation, &Sources) || !Sources.IsValidIndex(Item.SourceIndex))
        {
            Plan->Error += TEXT("无法确认实际重导入来源槽位：") + Pair.Key.ToString() + TEXT("\n");
            continue;
        }
        Item.OriginalSource = Sources[Item.SourceIndex];
        Item.bChangesSource = NormalizePath(Item.OriginalSource) != NormalizePath(Item.TargetFile);
        Item.Skeleton = Animation->GetSkeleton();
        Item.Dirty = InspectDirtyAssets(*Item.Row->Asset);
        Item.Row->Dirty = Item.Dirty;
    }
    // 所有所选对象加载并完成编译后才定稿，避免后加载的共享骨骼修改遗漏在确认范围之外。
    for (auto& Pair : Plan->Items)
    {
        FPlannedUpdate& Item = Pair.Value;
        if (!Item.Animation.IsValid() || !Item.Skeleton.IsValid()) { continue; }
        Item.Dirty = InspectDirtyAssets(*Item.Row->Asset);
        Item.Row->Dirty = Item.Dirty;
        for (UPackage* Package : {Item.Animation->GetPackage(), Item.Skeleton->GetPackage()})
        {
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
            TEXT("\n是否脏数据：") + Item.Dirty.Label + TEXT("\n") + Item.Dirty.Tooltip);
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
        if (Item.bChangesSource)
        {
            Sources.Add(Pair.Key.ToString() + TEXT("\n原来源：") + Item.OriginalSource + TEXT("\n本次来源：") + Item.TargetFile);
        }
        if (Item.Dirty.bAnimationDirty) { DirtyAssets.Add(TEXT("动画：") + Item.Dirty.AnimationPath); }
        if (Item.Dirty.bSkeletonDirty) { DirtyAssets.Add(TEXT("Skeleton：") + Item.Dirty.SkeletonPath); }
    }
    if (Sources.IsEmpty() && DirtyAssets.IsEmpty()) { return {}; }
    Sources.Sort();
    TArray<FString> Dirty = DirtyAssets.Array();
    Dirty.Sort();
    FString Message = FString::Printf(TEXT("更新 %d 个动画，仅修改内存，不自动保存或签出。完整计划已复制到剪贴板。\n"), Plan.Items.Num());
    if (!Sources.IsEmpty())
    {
        Message += FString::Printf(TEXT("\n来源改绑（%d 项）：\n%s%s\n"), Sources.Num(),
            *FString::Join(TArray<FString>(Sources.GetData(), FMath::Min(Sources.Num(), 5)), TEXT("\n\n")), Sources.Num() > 5 ? TEXT("\n其余见剪贴板") : TEXT(""));
    }
    if (!Dirty.IsEmpty())
    {
        Message += FString::Printf(TEXT("\n将覆盖已有未保存修改的资产（%d 个，已去重）：\n%s%s\n"), Dirty.Num(),
            *FString::Join(TArray<FString>(Dirty.GetData(), FMath::Min(Dirty.Num(), 10)), TEXT("\n")), Dirty.Num() > 10 ? TEXT("\n其余见剪贴板") : TEXT(""));
    }
    return Message + TEXT("\n将按原生重导入写入数据，可能覆盖已有编辑内容，不再因已确认的脏状态而跳过；不是先清空整个 Skeleton。") +
        TEXT("动画、Skeleton 及其他依赖由你检查后手动保存，不保证可以一次撤销。请勿并行编辑相关资产或运行其他导入任务。继续？");
}

bool IsPlannedUpdateCurrent(const FReimportPlan& Plan, const FPlannedUpdate& Item, FString& Error)
{
    const UAnimSequence* Animation = Item.Animation.Get();
    if (!Animation || Animation->GetSkeleton() != Item.Skeleton.Get() || !Animation->AssetImportData ||
        SourceFingerprint(FAssetData(Animation)) != Item.SourceJson || Item.Row->SourceIndex != Item.SourceIndex ||
        NormalizePath(Item.Row->File) != NormalizePath(Item.TargetFile))
    {
        Error = TEXT("动画、Skeleton 或来源信息已变化，请重新检索并确认");
        return false;
    }
    for (UPackage* Package : {Animation->GetPackage(), Item.Skeleton->GetPackage()})
    {
        const uint64* Accepted = Plan.AcceptedRevisions.Find(Package);
        if (!Accepted || *Accepted != Plan.Changes->Get(Package) || Package->IsDirty() != Plan.AcceptedDirtyStates.FindRef(Package))
        {
            Error = TEXT("出现确认范围之外的新修改，请重新确认：") + Package->GetName();
            return false;
        }
    }
    const auto Sources = Animation->AssetImportData->ExtractFilenames();
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
