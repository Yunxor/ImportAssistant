#include "ImportAssistantReimportQueue.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetCompilingManager.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "HAL/FileManager.h"
#include "ImportAssistantMatcher.h"
#include "InterchangeResultsContainer.h"
#include "InterchangeAssetImportData.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

namespace ImportAssistant
{
FReimportQueue::~FReimportQueue()
{
    if (Result.IsValid()) { Result->WaitUntilDone(); }
    ReleaseItem();
    EndBatch();
}

void FReimportQueue::Start(const TArray<FRowPtr>& Rows, TSharedPtr<FReimportPlan> ApprovedPlan)
{
    EndBatch();
    Plan = MoveTemp(ApprovedPlan);
    Items.Reset();
    TSet<FSoftObjectPath> Seen;
    for (const FRowPtr& Row : Rows)
    {
        if (Row->bSelected && Row->CanSelect() && !Seen.Contains(Row->Asset->ObjectPath))
        {
            Seen.Add(Row->Asset->ObjectPath);
            Row->Status = TEXT("等待执行");
            Items.Add(Row);
        }
    }
    Next = 0;
    bCancel = false;
    StopReason.Reset();
    bRunning = !Items.IsEmpty();
    if (!bRunning) { return; }
    DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddLambda([this](UPackage* Package, bool)
    {
        FScopeLock Lock(&DirtyMutex);
        if (bTrackingImport) { DirtiedPackages.Add(Package); }
    });
}

void FReimportQueue::Cancel()
{
    if (bRunning && !bCancel) { bCancel = true; StopReason = TEXT("用户取消，未执行"); }
}

FString FReimportQueue::GetStatus() const
{
    return FString::Printf(TEXT("%s：%d / %d%s"), bRunning ? TEXT("更新进度（不保存）") : TEXT("本轮结束（未自动保存）"),
        Next - (Current.IsValid() ? 1 : 0), Items.Num(), bCancel ? TEXT("；停止后续更新") : TEXT(""));
}

void FReimportQueue::Tick()
{
    if (!bRunning) { return; }
    if (Result.IsValid())
    {
        if (Result->GetStatus() == UE::Interchange::FImportResult::EStatus::InProgress) { return; }
        FinishItem();
        ReleaseItem();
        return;
    }
    if (bCancel)
    {
        for (; Next < Items.Num(); ++Next) { Items[Next]->Status = StopReason; }
        EndBatch();
        return;
    }
    if (Next == Items.Num()) { EndBatch(); return; }
    // 避免在另一个 Interchange 任务尚未完成时再次发起更新。
    if (UInterchangeManager::IsImporting() || (GEditor && GEditor->PlayWorld)) { return; }
    BeginItem();
    if (!Result.IsValid()) { ReleaseItem(); }
}

void FReimportQueue::BeginItem()
{
    Current = Items[Next++];
    Current->Status = TEXT("加载并复核资产…");
    Animation.Reset(Cast<UAnimSequence>(Current->Asset->ObjectPath.TryLoad()));
    if (!Animation.IsValid() || !Animation->AssetImportData || !Animation->GetSkeleton())
    {
        Current->Status = TEXT("跳过：资产不存在、缺少导入数据或 Skeleton");
        return;
    }
    if (!Plan.IsValid())
    {
        Current->Status = TEXT("已中止：缺少执行前风险预检，请重新确认");
        bCancel = true;
        StopReason = TEXT("执行前状态变化，需重新确认，未执行");
        return;
    }
    const FPlannedUpdate* Approved = Plan->Items.Find(Current->Asset->ObjectPath);
    FString Changed;
    if (!Approved || !Plan->Error.IsEmpty() || !IsPlannedUpdateCurrent(*Plan, *Approved, Changed))
    {
        Current->Status = TEXT("已中止：") + (Changed.IsEmpty() ? TEXT("执行计划无效，请重新确认") : Changed);
        bCancel = true;
        StopReason = TEXT("执行前状态变化，需重新确认，未执行");
        return;
    }
    if (!IFileManager::Get().FileExists(*Current->File))
    {
        Current->Status = TEXT("失败：本次选定 FBX 不存在");
        return;
    }
    const bool bInterchange = Animation->AssetImportData->IsA<UInterchangeAssetImportData>();
    if (!bInterchange && !Animation->AssetImportData->IsA<UFbxAnimSequenceImportData>())
    {
        Current->Status = TEXT("跳过：尚未支持此导入数据类型，避免自动转换导入配置");
        return;
    }
    if (bInterchange)
    {
        const TArray<FString> Formats = UInterchangeManager::GetInterchangeManager().GetSupportedAssetTypeFormats(EInterchangeTranslatorAssetType::Animations);
        if (!UInterchangeManager::IsInterchangeImportEnabled() || !Formats.ContainsByPredicate([](const FString& Format)
            { return Format.StartsWith(TEXT("fbx;"), ESearchCase::IgnoreCase); }))
        {
            Current->Status = TEXT("跳过：Interchange FBX 未启用，禁止自动转换到旧导入器");
            return;
        }
    }
    if (!bInterchange && Current->SourceIndex != 0)
    {
        Current->Status = TEXT("跳过：旧 FBX 动画工厂只支持主来源槽位");
        return;
    }
    // 仅在旧工厂同步调用期间关闭 Interchange，不能让全局开关跨帧保持改变。
    TUniquePtr<UE::Interchange::FScopedInterchangeImportEnableState> LegacyImportMode;
    if (!bInterchange) { LegacyImportMode = MakeUnique<UE::Interchange::FScopedInterchangeImportEnableState>(false); }
    OriginalImportClass = Animation->AssetImportData->GetClass();
    TArray<FString> Sources;
    if (!FReimportManager::Instance()->CanReimport(Animation.Get(), &Sources) || !Sources.IsValidIndex(Current->SourceIndex))
    {
        Current->Status = TEXT("跳过：当前导入后端不支持该资产或来源槽位");
        return;
    }
    if (NormalizePath(Sources[Current->SourceIndex]) != NormalizePath(Approved->OriginalSource))
    {
        Current->Status = TEXT("已中止：实际导入后端来源已变化，请重新确认");
        bCancel = true;
        StopReason = TEXT("执行前状态变化，需重新确认，未执行");
        return;
    }
    {
        FScopeLock Lock(&DirtyMutex);
        bTrackingImport = true;
    }
    OldSource = Sources[Current->SourceIndex];
    bChangedSource = NormalizePath(OldSource) != NormalizePath(Current->File);
    if (bChangedSource)
    {
        FReimportManager::Instance()->UpdateReimportPath(Animation.Get(), Current->File, Current->SourceIndex);
        Sources.Reset();
        FReimportManager::Instance()->CanReimport(Animation.Get(), &Sources);
        if (!Sources.IsValidIndex(Current->SourceIndex) || NormalizePath(Sources[Current->SourceIndex]) != NormalizePath(Current->File))
        {
            FReimportManager::Instance()->UpdateReimportPath(Animation.Get(), OldSource, Current->SourceIndex);
            Current->Status = TEXT("失败：导入后端未接受指定来源，已尝试恢复路径；请检查未保存资产");
            return;
        }
    }
    Current->Status = TEXT("正在重导入…");
    Result = FReimportManager::Instance()->ReimportAsync(Animation.Get(), false, false, Current->File,
        nullptr, Current->SourceIndex, false, true, false);
}

void FReimportQueue::FinishItem()
{
    TUniquePtr<UE::Interchange::FScopedInterchangeImportEnableState> LegacyImportMode;
    if (OriginalImportClass.Get() == UFbxAnimSequenceImportData::StaticClass())
    {
        LegacyImportMode = MakeUnique<UE::Interchange::FScopedInterchangeImportEnableState>(false);
    }
    TArray<FString> Messages;
    bool bFailed = Result->GetStatus() != UE::Interchange::FImportResult::EStatus::Done;
    for (const UInterchangeResult* Message : Result->GetResults()->GetResults())
    {
        bFailed |= Message->GetResultType() == EInterchangeResultType::Error;
        if (Message->GetResultType() != EInterchangeResultType::Success) { Messages.Add(Message->GetText().ToString()); }
    }
    if (Animation->AssetImportData->GetClass() != OriginalImportClass.Get())
    {
        bFailed = true;
        Messages.Add(TEXT("导入后端改变了导入数据类型，未保存，请检查资产"));
    }
    if (!bFailed)
    {
        const TArray<FString> FinalSources = Animation->AssetImportData->ExtractFilenames();
        if (!FinalSources.IsValidIndex(Current->SourceIndex) || NormalizePath(FinalSources[Current->SourceIndex]) != NormalizePath(Current->File) ||
            Animation->GetNumberOfSampledKeys() == 0)
        {
            bFailed = true;
            Messages.Add(TEXT("导入完成但来源或动画关键帧检查失败"));
        }
    }
    if (bFailed)
    {
        if (bChangedSource) { FReimportManager::Instance()->UpdateReimportPath(Animation.Get(), OldSource, Current->SourceIndex); }
        Current->Status = TEXT("导入失败，未保存；对象可能已被修改，请检查或从版本控制恢复。") + FString::Join(Messages, TEXT("；"));
        return;
    }
    UObject* Objects[] = { Animation.Get() };
    FAssetCompilingManager::Get().FinishCompilationForObjects(Objects);
    // 只更新内存并保留脏标记；动画、Skeleton 和其他依赖均交给用户决定何时保存。
    Animation->MarkPackageDirty();
    UPackage* AnimationPackage = Animation->GetPackage();
    UPackage* SkeletonPackage = Animation->GetSkeleton()->GetPackage();
    TArray<TWeakObjectPtr<UPackage>> Modified;
    {
        FScopeLock Lock(&DirtyMutex);
        bTrackingImport = false;
        Modified = DirtiedPackages.Array();
    }
    bItemSucceeded = true;
    // 只接受本项导入自身产生的动画和骨骼变更，项间的外部编辑仍会使许可失效。
    for (UPackage* Package : {AnimationPackage, SkeletonPackage})
    {
        Plan->AcceptedRevisions.Add(Package, Plan->Changes->Get(Package));
        Plan->AcceptedDirtyStates.Add(Package, Package->IsDirty());
    }
    if (SkeletonPackage != AnimationPackage && SkeletonPackage->IsDirty())
    {
        Messages.Add(TEXT("Skeleton 待手动保存：") + SkeletonPackage->GetName());
    }
    TArray<FString> Dependencies;
    for (const TWeakObjectPtr<UPackage>& WeakPackage : Modified)
    {
        UPackage* Package = WeakPackage.Get();
        if (Package && Package != AnimationPackage && Package != SkeletonPackage && Package->IsDirty())
        {
            Dependencies.Add(Package->GetName());
        }
    }
    Dependencies.Sort();
    for (const FString& PackageName : Dependencies) { Messages.Add(TEXT("其他受影响包请检查后手动保存：") + PackageName); }
    Current->Status = TEXT("已更新，未保存");
    if (!Messages.IsEmpty()) { Current->Status += TEXT("；") + FString::Join(Messages, TEXT("；")); }
}

void FReimportQueue::ReleaseItem()
{
    {
        FScopeLock Lock(&DirtyMutex);
        // 导入失败不接受其残留变更；停止后续项，避免在失败数据上继续覆盖共享骨骼。
        if (bTrackingImport && !bItemSucceeded)
        {
            bCancel = true;
            StopReason = TEXT("前项导入失败，批次已停止，未执行");
        }
        bTrackingImport = false;
        DirtiedPackages.Reset();
    }
    Result.Reset();
    Animation.Reset();
    Current.Reset();
    OldSource.Reset();
    bChangedSource = false;
    bItemSucceeded = false;
    OriginalImportClass.Reset();
}

void FReimportQueue::EndBatch()
{
    if (DirtyHandle.IsValid()) { UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle); DirtyHandle.Reset(); }
    FScopeLock Lock(&DirtyMutex);
    bRunning = false;
    bTrackingImport = false;
    Plan.Reset();
}
}


