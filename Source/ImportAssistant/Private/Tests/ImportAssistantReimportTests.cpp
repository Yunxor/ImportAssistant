#include "ImportAssistantReimportQueue.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorReimportHandler.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "HAL/FileManager.h"
#include "ImportAssistantMatcher.h"
#include "SImportAssistantWindow.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"

using namespace ImportAssistant;

namespace
{
// 仅处理本测试创建的内存动画，替代 FBX 解析，验证真实队列的导入、脏状态和不保存行为。
class FMemoryReimportHandler : public FReimportHandler
{
public:
    TSet<UObject*> Assets;
    UPackage* ExtraPackage = nullptr;
    int32 Count = 0;
    int32 FailOnCall = INDEX_NONE;
    bool bUpdateSourceMetadata = false;

    virtual int32 GetPriority() const override { return MAX_int32; }
    virtual bool CanReimport(UObject* Object, TArray<FString>& Filenames) override
    {
        if (!Assets.Contains(Object)) { return false; }
        CastChecked<UAnimSequence>(Object)->AssetImportData->ExtractFilenames(Filenames);
        return true;
    }
    virtual void SetReimportPaths(UObject* Object, const TArray<FString>& Filenames) override
    {
        if (Assets.Contains(Object) && !Filenames.IsEmpty()) { CastChecked<UAnimSequence>(Object)->AssetImportData->UpdateFilenameOnly(Filenames[0]); }
    }
    virtual EReimportResult::Type Reimport(UObject* Object) override
    {
        ++Count;
        UAnimSequence* Animation = CastChecked<UAnimSequence>(Object);
        Animation->RateScale = 2.0f;
        if (bUpdateSourceMetadata) { Animation->AssetImportData->Update(Animation->AssetImportData->GetFirstFilename()); }
        Animation->MarkPackageDirty();
        Animation->GetSkeleton()->MarkPackageDirty();
        if (ExtraPackage) { ExtraPackage->MarkPackageDirty(); }
        return Count == FailOnCall ? EReimportResult::Failed : EReimportResult::Succeeded;
    }
};

struct FQueueFixture
{
    FString Root;
    FString Source;
    TStrongObjectPtr<USkeleton> Skeleton;
    TStrongObjectPtr<UPackage> Extra;
    TArray<TStrongObjectPtr<UAnimSequence>> Animations;
    TArray<FRowPtr> Rows;
    FMemoryReimportHandler Handler;
    FReimportQueue Queue;
    FDelegateHandle SaveHandle;
    int32 SaveCount = 0;
    bool bSourceWritten = false;

    explicit FQueueFixture(int32 AnimationCount)
    {
        const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Root = TEXT("/Game/__ImportAssistantUpdateOnly_") + Id;
        const FString Directory = FPaths::ProjectSavedDir() / TEXT("ImportAssistantTests") / Id;
        IFileManager::Get().MakeDirectory(*Directory, true);
        Source = FPaths::ConvertRelativePathToFull(Directory / TEXT("MemoryOnly.fbx"));
        bSourceWritten = FFileHelper::SaveStringToFile(TEXT("仅供内存导入处理器测试，不是真实 FBX"), *Source);
        Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root / TEXT("Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
        {
            FReferenceSkeletonModifier Modifier(Skeleton.Get());
            Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
        }
        Extra.Reset(CreatePackage(*(Root / TEXT("Dependency"))));
        Handler.ExtraPackage = Extra.Get();
        for (int32 Index = 0; Index < AnimationCount; ++Index)
        {
            const FString Name = FString::Printf(TEXT("Animation%d"), Index);
            TStrongObjectPtr<UAnimSequence> Animation(NewObject<UAnimSequence>(CreatePackage(*(Root / Name)), *Name, RF_Public | RF_Standalone));
            Animation->SetSkeleton(Skeleton.Get());
            IAnimationDataController& Controller = Animation->GetController();
            Controller.InitializeModel();
            Controller.SetFrameRate(FFrameRate(30, 1), false);
            Controller.SetNumberOfFrames(1, false);
            Controller.NotifyPopulated();
            UObject* Objects[] = {Animation.Get()};
            FAssetCompilingManager::Get().FinishCompilationForObjects(Objects);
            Animation->AssetImportData = NewObject<UFbxAnimSequenceImportData>(Animation.Get());
            Animation->AssetImportData->SourceData.SourceFiles.Emplace(Source);
            Animation->GetPackage()->SetDirtyFlag(false);
            FRowPtr Row = MakeShared<FMatchRow>();
            Row->Asset = MakeRecord(FAssetData(Animation.Get()), TEXT(""));
            Row->File = Source;
            Row->SourceIndex = 0;
            Row->bSelected = true;
            Row->Reason = EMatchReason::Exact;
            Rows.Add(Row);
            Handler.Assets.Add(Animation.Get());
            Animations.Add(MoveTemp(Animation));
        }
        Skeleton->GetPackage()->SetDirtyFlag(false);
        Extra->SetDirtyFlag(false);
        SaveHandle = UPackage::PreSavePackageWithContextEvent.AddLambda([this](UPackage* Package, FObjectPreSaveContext)
        {
            if (Package->GetName().StartsWith(Root)) { ++SaveCount; }
        });
    }

    ~FQueueFixture()
    {
        Queue.Cancel();
        Queue.Tick();
        Queue.Tick();
        UPackage::PreSavePackageWithContextEvent.Remove(SaveHandle);
        for (auto& Animation : Animations)
        {
            FAssetRegistryModule::AssetDeleted(Animation.Get());
            Animation->ClearFlags(RF_Public | RF_Standalone);
            Animation->SetFlags(RF_Transient);
            Animation->GetPackage()->SetDirtyFlag(false);
            Animation->GetPackage()->SetFlags(RF_Transient);
        }
        Skeleton->ClearFlags(RF_Public | RF_Standalone);
        Skeleton->SetFlags(RF_Transient);
        Skeleton->GetPackage()->SetDirtyFlag(false);
        Skeleton->GetPackage()->SetFlags(RF_Transient);
        Extra->SetDirtyFlag(false);
        Extra->SetFlags(RF_Transient);
    }

    bool Run()
    {
        Queue.Start(Rows, BuildReimportPlan(Rows));
        for (int32 Step = 0; Step < Rows.Num() * 3 + 3 && Queue.IsRunning(); ++Step) { Queue.Tick(); }
        return !Queue.IsRunning();
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantUpdateOnlyTest, "ImportAssistant.Reimport.UpdateOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantUpdateOnlyTest::RunTest(const FString&)
{
    FQueueFixture Fixture(2);
    if (!TestTrue(TEXT("测试源文件已创建"), Fixture.bSourceWritten)) { return false; }
    TestTrue(TEXT("更新队列能够完成"), Fixture.Run());
    TestEqual(TEXT("共享 Skeleton 两个动画都更新"), Fixture.Handler.Count, 2);
    TestEqual(TEXT("不触发任何测试资产保存"), Fixture.SaveCount, 0);
    for (int32 Index = 0; Index < 2; ++Index)
    {
        TestEqual(TEXT("实际执行内存更新"), Fixture.Animations[Index]->RateScale, 2.0f);
        TestTrue(TEXT("动画保持待保存状态"), Fixture.Animations[Index]->GetPackage()->IsDirty());
        TestTrue(TEXT("结果不冒充保存成功"), Fixture.Rows[Index]->Status.StartsWith(TEXT("已更新，未保存")));
        TestFalse(TEXT("未创建磁盘 uasset"), FPackageName::DoesPackageExist(Fixture.Animations[Index]->GetPackage()->GetName()));
        TestTrue(TEXT("报告 Skeleton 待保存"), Fixture.Rows[Index]->Status.Contains(TEXT("Skeleton 待手动保存")));
        TestTrue(TEXT("报告其他依赖而不保存"), Fixture.Rows[Index]->Status.Contains(TEXT("Dependency")));
    }
    TestTrue(TEXT("共享 Skeleton 未被保存清理"), Fixture.Skeleton->GetPackage()->IsDirty());
    TestTrue(TEXT("额外依赖未被保存清理"), Fixture.Extra->IsDirty());
    Fixture.Queue.Start(Fixture.Rows, BuildReimportPlan(Fixture.Rows));
    for (int32 Step = 0; Step < 8 && Fixture.Queue.IsRunning(); ++Step) { Fixture.Queue.Tick(); }
    TestEqual(TEXT("新一批重新预检并确认后可再次覆盖脏数据"), Fixture.Handler.Count, 4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantDirtyPolicyTest, "ImportAssistant.Reimport.DirtyPackagePolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantDirtyPolicyTest::RunTest(const FString&)
{
    {
        FQueueFixture Fixture(2);
        Fixture.Skeleton->MarkPackageDirty();
        TestTrue(TEXT("预先修改 Skeleton 的队列正常结束"), Fixture.Run());
        TestEqual(TEXT("确认后不跳过批次前 Skeleton 修改"), Fixture.Handler.Count, 2);
        TestTrue(TEXT("保留原 Skeleton 脏状态"), Fixture.Skeleton->GetPackage()->IsDirty());
    }
    {
        FQueueFixture Fixture(2);
        Fixture.Animations[0]->MarkPackageDirty();
        TestTrue(TEXT("单个预先修改动画不会阻塞其他独立项"), Fixture.Run());
        TestEqual(TEXT("确认后脏动画和干净动画都更新"), Fixture.Handler.Count, 2);
        TestTrue(TEXT("原修改允许覆盖并保留待保存结果"), Fixture.Rows[0]->Status.StartsWith(TEXT("已更新，未保存")));
    }
    {
        FQueueFixture Fixture(2);
        Fixture.Queue.Start(Fixture.Rows, BuildReimportPlan(Fixture.Rows));
        Fixture.Queue.Tick();
        Fixture.Queue.Tick();
        Fixture.Skeleton->MarkPackageDirty();
        for (int32 Step = 0; Step < 5 && Fixture.Queue.IsRunning(); ++Step) { Fixture.Queue.Tick(); }
        TestEqual(TEXT("项目之间的外部 Skeleton 编辑使本批许可失效"), Fixture.Handler.Count, 1);
        TestTrue(TEXT("后续项中止并要求重新确认"), Fixture.Rows[1]->Status.StartsWith(TEXT("已中止：")));
    }
    {
        FQueueFixture Fixture(3);
        Fixture.Rows[1]->File = Fixture.Source + TEXT(".missing");
        TestTrue(TEXT("源文件缺失后继续其他项目"), Fixture.Run());
        TestEqual(TEXT("导入前跳过不撤销已有共享 Skeleton 许可"), Fixture.Handler.Count, 2);
        TestTrue(TEXT("第三个动画依然更新且未保存"), Fixture.Rows[2]->Status.StartsWith(TEXT("已更新，未保存")));
        TestEqual(TEXT("跳过缺文件项不产生保存"), Fixture.SaveCount, 0);
    }
    {
        FQueueFixture Fixture(3);
        Fixture.Handler.FailOnCall = 2;
        TestTrue(TEXT("失败后队列仍能结束"), Fixture.Run());
        TestEqual(TEXT("失败留下脏 Skeleton 时不更新第三个动画"), Fixture.Handler.Count, 2);
        TestTrue(TEXT("失败项未保存"), Fixture.Rows[1]->Status.StartsWith(TEXT("导入失败，未保存")));
        TestTrue(TEXT("失败后不继续写入其他项"), Fixture.Rows[2]->Status.Contains(TEXT("未执行")));
        TestEqual(TEXT("失败路径也不保存"), Fixture.SaveCount, 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantCancelUpdateTest, "ImportAssistant.Reimport.CancelLeavesUnsaved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantCancelUpdateTest::RunTest(const FString&)
{
    FQueueFixture Fixture(2);
    TestFalse(TEXT("空闲队列没有停止请求"), Fixture.Queue.IsStopRequested());
    Fixture.Queue.Cancel();
    TestFalse(TEXT("空闲取消不产生停止中状态"), Fixture.Queue.IsStopRequested());
    Fixture.Queue.Start(Fixture.Rows, BuildReimportPlan(Fixture.Rows));
    TestFalse(TEXT("新批次清除停止请求"), Fixture.Queue.IsStopRequested());
    Fixture.Queue.Tick();
    Fixture.Queue.Cancel();
    TestTrue(TEXT("当前项收尾前仍在运行并已请求停止"), Fixture.Queue.IsRunning() && Fixture.Queue.IsStopRequested());
    Fixture.Queue.Cancel();
    TestTrue(TEXT("重复停止不会切回运行状态"), Fixture.Queue.IsStopRequested());
    for (int32 Step = 0; Step < 5 && Fixture.Queue.IsRunning(); ++Step) { Fixture.Queue.Tick(); }
    TestFalse(TEXT("停止完成后不再返回停止中状态"), Fixture.Queue.IsStopRequested());
    TestFalse(TEXT("取消后停止后续项"), Fixture.Queue.IsRunning());
    TestEqual(TEXT("只有当前动画更新"), Fixture.Handler.Count, 1);
    TestTrue(TEXT("当前动画保留已更新未保存结果"), Fixture.Rows[0]->Status.StartsWith(TEXT("已更新，未保存")));
    TestEqual(TEXT("后续动画显示取消"), Fixture.Rows[1]->Status, FString(TEXT("用户取消，未执行")));
    TestTrue(TEXT("取消不清除已完成项的修改"), Fixture.Animations[0]->GetPackage()->IsDirty());
    TestEqual(TEXT("取消路径不保存"), Fixture.SaveCount, 0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantDirtyStateTest, "ImportAssistant.AssetState.DirtyInspection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantDirtyStateTest::RunTest(const FString&)
{
    FQueueFixture Fixture(2);
    const FAssetRecord& Asset = *Fixture.Rows[0]->Asset;
    TestEqual(TEXT("干净动画和骨骼显示否"), InspectDirtyAssets(Asset).Label, FString(TEXT("否")));
    Fixture.Animations[0]->MarkPackageDirty();
    auto Info = InspectDirtyAssets(Asset);
    TestEqual(TEXT("只修改动画时准确标注"), Info.Label, FString(TEXT("是：动画")));
    TestTrue(TEXT("动画 Tooltip 指向具体资产"), Info.Tooltip.Contains(Fixture.Animations[0]->GetPathName()));
    TestFalse(TEXT("单元格不重复脏状态概念说明"), Info.Tooltip.Contains(TEXT("不表示数据损坏")) || Info.Tooltip.Contains(TEXT("列表检查不加载")));
    Fixture.Skeleton->MarkPackageDirty();
    Info = InspectDirtyAssets(Asset);
    TestEqual(TEXT("同时修改动画骨骼"), Info.Label, FString(TEXT("是：动画、骨骼")));
    TestTrue(TEXT("Tooltip 指向具体共享 Skeleton"), Info.Tooltip.Contains(Fixture.Skeleton->GetPathName()));
    TestEqual(TEXT("共享 Skeleton 的其他动画同样标脏"), InspectDirtyAssets(*Fixture.Rows[1]->Asset).Label, FString(TEXT("是：骨骼")));
    Fixture.Animations[0]->GetPackage()->SetDirtyFlag(false);
    Fixture.Skeleton->GetPackage()->SetDirtyFlag(false);
    TestEqual(TEXT("保存后状态恢复，测试只模拟清脏不写资产"), InspectDirtyAssets(Asset).Label, FString(TEXT("否")));
    FAssetRecord Unloaded;
    Unloaded.ObjectPath = FSoftObjectPath(Fixture.Root / TEXT("NotLoaded.NotLoaded"));
    Unloaded.Skeleton = TEXT("/Script/Engine.Skeleton'") + Fixture.Skeleton->GetPathName() + TEXT("'");
    Fixture.Skeleton->MarkPackageDirty();
    TestTrue(TEXT("未加载动画仍按元数据找到脏骨骼"), InspectDirtyAssets(Unloaded).bSkeletonDirty);
    TestTrue(TEXT("列表状态检查不加载缺失动画"), Unloaded.ObjectPath.ResolveObject() == nullptr);
    Unloaded.Skeleton.Reset();
    TestFalse(TEXT("缺骨骼关联不伪报全部干净"), InspectDirtyAssets(Unloaded).bKnown);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantRiskWarningTest, "ImportAssistant.AssetState.RiskWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantRiskWarningTest::RunTest(const FString&)
{
    FQueueFixture Fixture(2);
    auto Plan = BuildReimportPlan(Fixture.Rows);
    TestTrue(TEXT("同源且干净时不产生计划弹窗"), Plan->Error.IsEmpty() && BuildReimportWarning(*Plan).IsEmpty());
    Fixture.Rows[0]->File.ReplaceInline(TEXT("/"), TEXT("\\"));
    Fixture.Rows[0]->File.ToUpperInline();
    Plan = BuildReimportPlan(Fixture.Rows);
    TestTrue(TEXT("分隔符及大小写不同不算改绑"), BuildReimportWarning(*Plan).IsEmpty());
    Fixture.Rows[0]->File = Fixture.Source;
    FString PackageFile;
    FPackageName::TryConvertLongPackageNameToFilename(Fixture.Animations[0]->GetPackage()->GetName(), PackageFile);
    FString Relative = Fixture.Source;
    const FString PackageDirectory = FPaths::ConvertRelativePathToFull(FPaths::GetPath(PackageFile)) + TEXT("/");
    FPaths::MakePathRelativeTo(Relative, *PackageDirectory);
    Fixture.Animations[0]->AssetImportData->SourceData.SourceFiles[0].RelativeFilename = Relative;
    Fixture.Rows[0]->Asset = MakeRecord(FAssetData(Fixture.Animations[0].Get()), TEXT(""));
    Plan = BuildReimportPlan(Fixture.Rows);
    TestTrue(TEXT("包相对来源解析到同一文件时不误报改绑"), Plan->Error.IsEmpty() && BuildReimportWarning(*Plan).IsEmpty());
    Fixture.Skeleton->MarkPackageDirty();
    Plan = BuildReimportPlan(Fixture.Rows);
    const FString Warning = BuildReimportWarning(*Plan);
    TestTrue(TEXT("勾选脏骨骼出现覆盖提示"), Warning.Contains(TEXT("将覆盖已有未保存修改")) && Warning.Contains(TEXT("不再因已确认的脏状态而跳过")));
    TestTrue(TEXT("同一骨骼风险列表只列一次"), Warning.Contains(TEXT("1 个，已去重")));
    TestTrue(TEXT("完整计划包含脏资产详情"), DescribeReimportPlan(*Plan).Contains(Fixture.Skeleton->GetPathName()));
    Fixture.Skeleton->GetPackage()->SetDirtyFlag(false);
    Fixture.Rows[0]->File = Fixture.Source + TEXT(".replacement.fbx");
    Plan = BuildReimportPlan(Fixture.Rows);
    TestTrue(TEXT("仅来源改绑也触发提示"), BuildReimportWarning(*Plan).Contains(TEXT("来源改绑或分项内容更新（1 项）")));
    Fixture.Rows[0]->bSelected = false;
    Fixture.Animations[0]->MarkPackageDirty();
    Plan = BuildReimportPlan(Fixture.Rows);
    TestTrue(TEXT("未勾选的改绑及脏动画不触发提示"), BuildReimportWarning(*Plan).IsEmpty());
    TestEqual(TEXT("预检及提示逻辑不保存资产"), Fixture.SaveCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantRiskChangesTest, "ImportAssistant.Reimport.UnconfirmedChanges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantRiskChangesTest::RunTest(const FString&)
{
    {
        FQueueFixture Fixture(2);
        Fixture.Skeleton->MarkPackageDirty();
        const auto Plan = BuildReimportPlan(Fixture.Rows);
        FString Error;
        TestTrue(TEXT("已脏状态的预检计划可执行"), IsReimportPlanCurrent(*Plan, Error));
        Fixture.Skeleton->MarkPackageDirty();
        TestFalse(TEXT("已脏的骨骼继续修改也使确认过期"), IsReimportPlanCurrent(*Plan, Error));
        Fixture.Queue.Start(Fixture.Rows, Plan);
        for (int32 Step = 0; Step < 6 && Fixture.Queue.IsRunning(); ++Step) { Fixture.Queue.Tick(); }
        TestEqual(TEXT("不覆盖确认期间的新修改"), Fixture.Handler.Count, 0);
        TestTrue(TEXT("未确认变更要求重新确认"), Fixture.Rows[0]->Status.Contains(TEXT("重新确认")));
    }
    {
        FQueueFixture Fixture(2);
        const auto Plan = BuildReimportPlan(Fixture.Rows);
        Fixture.Animations[0]->AssetImportData->UpdateFilenameOnly(Fixture.Source + TEXT(".changed"));
        Fixture.Queue.Start(Fixture.Rows, Plan);
        for (int32 Step = 0; Step < 6 && Fixture.Queue.IsRunning(); ++Step) { Fixture.Queue.Tick(); }
        TestEqual(TEXT("确认后来源元数据变化停止批次"), Fixture.Handler.Count, 0);
        TestTrue(TEXT("来源变更不是静默覆盖"), Fixture.Rows[0]->Status.Contains(TEXT("来源信息已变化")));
    }
    {
        FQueueFixture Fixture(1);
        const auto Plan = BuildReimportPlan(Fixture.Rows);
        Fixture.Animations[0]->GetPackage()->SetDirtyFlag(true);
        FString Error;
        TestFalse(TEXT("即使没有 MarkPackageDirty 事件也识别新脏状态"), IsReimportPlanCurrent(*Plan, Error));
        Fixture.Animations[0]->GetPackage()->SetDirtyFlag(false);
        Fixture.Animations[0]->MarkPackageDirty();
        const auto DirtyPlan = BuildReimportPlan(Fixture.Rows);
        Fixture.Animations[0]->GetPackage()->SetDirtyFlag(false);
        TestFalse(TEXT("保存或清脏后也要求新计划，避免复用旧风险快照"), IsReimportPlanCurrent(*DirtyPlan, Error));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantSourceRebindTest, "ImportAssistant.Reimport.ConfirmedSourceRebind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantSourceRebindTest::RunTest(const FString&)
{
    FQueueFixture Fixture(1);
    const FString Replacement = Fixture.Source + TEXT(".replacement.fbx");
    if (!TestTrue(TEXT("写入替代测试源"), FFileHelper::SaveStringToFile(TEXT("仅供内存处理器使用"), *Replacement))) { return false; }
    Fixture.Rows[0]->File = Replacement;
    Fixture.Animations[0]->RateScale = 0.75f;
    Fixture.Animations[0]->MarkPackageDirty();
    const auto Plan = BuildReimportPlan(Fixture.Rows);
    TestTrue(TEXT("同时记录改绑和脏动画风险"), BuildReimportWarning(*Plan).Contains(TEXT("来源改绑")) &&
        Plan->Items.FindChecked(Fixture.Rows[0]->Asset->ObjectPath).Dirty.bAssetDirty);
    Fixture.Queue.Start(Fixture.Rows, Plan);
    for (int32 Step = 0; Step < 6 && Fixture.Queue.IsRunning(); ++Step) { Fixture.Queue.Tick(); }
    TestEqual(TEXT("已确认脏动画的已有值确实被原生处理器覆盖"), Fixture.Animations[0]->RateScale, 2.0f);
    TestEqual(TEXT("实际改绑为本次选择的来源"), NormalizePath(Fixture.Animations[0]->AssetImportData->ExtractFilenames()[0]), NormalizePath(Replacement));
    TestTrue(TEXT("覆盖后仍待用户保存"), Fixture.Animations[0]->GetPackage()->IsDirty());
    TestEqual(TEXT("来源改绑及脏覆盖均不自动保存"), Fixture.SaveCount, 0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantContinuousUpdateTest, "ImportAssistant.Reimport.FreshPlanAfterUpdate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantContinuousUpdateTest::RunTest(const FString&)
{
    FQueueFixture Fixture(1);
    Fixture.Handler.bUpdateSourceMetadata = true;
    const FString OldSourceJson = Fixture.Rows[0]->Asset->SourceJson;
    TestTrue(TEXT("第一批更新成功"), Fixture.Run());
    const FString CurrentJson = SourceFingerprint(FAssetData(Fixture.Animations[0].Get()));
    TestTrue(TEXT("模拟真实重导入会刷新时间和哈希元数据"), CurrentJson != OldSourceJson);
    TestTrue(TEXT("普通检索快照失效仍需刷新"), !BuildReimportPlan(Fixture.Rows)->Error.IsEmpty());
    const auto FreshPlan = BuildReimportPlan(Fixture.Rows, true);
    TestTrue(TEXT("报告模式重新读取真实来源而不依赖旧快照"), FreshPlan->Error.IsEmpty() &&
        FreshPlan->Items.FindChecked(Fixture.Rows[0]->Asset->ObjectPath).SourceJson == CurrentJson);
    TestTrue(TEXT("第二批脏资产仍通过覆盖确认而非静默写入"), BuildReimportWarning(*FreshPlan).Contains(TEXT("将覆盖已有未保存修改")));
    FString Changed;
    TestTrue(TEXT("新计划可通过最终复核"), IsReimportPlanCurrent(*FreshPlan, Changed));
    Fixture.Queue.Start(Fixture.Rows, FreshPlan);
    for (int32 Step = 0; Step < 6 && Fixture.Queue.IsRunning(); ++Step) { Fixture.Queue.Tick(); }
    TestEqual(TEXT("无需重新检索即可成功执行第二批"), Fixture.Handler.Count, 2);
    TestTrue(TEXT("结果仍为待保存"), Fixture.Rows[0]->Status.StartsWith(TEXT("已更新，未保存")));
    TestEqual(TEXT("连续更新不覆盖报告的最初来源快照"), Fixture.Rows[0]->Asset->SourceJson, OldSourceJson);
    TestEqual(TEXT("连续批次均不保存"), Fixture.SaveCount, 0);
    const auto LatestPlan = BuildReimportPlan(Fixture.Rows, true);
    Fixture.Skeleton->MarkPackageDirty();
    TestFalse(TEXT("报告模式也不能绕过新计划确认后的额外修改"), IsReimportPlanCurrent(*LatestPlan, Changed));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantVersionRefreshTest, "ImportAssistant.Editor.ImportedVersionRefresh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantVersionRefreshTest::RunTest(const FString&)
{
    FQueueFixture Fixture(1);
    const auto Window = SNew(SImportAssistantWindow).LoadSavedSettings(false);
    const FRowPtr Row = Fixture.Rows[0];
    Window->Rows = {Row};
    Row->Status = TEXT("已更新，未保存");
    Window->bShowReport = true;
    const FString OriginalJson = Row->Asset->SourceJson;
    Fixture.Animations[0]->AssetImportData->SourceData.SourceFiles[0].Timestamp = FDateTime(2026, 11, 8, 5, 6, 7);
    Window->RefreshVersionTimes();
    TestEqual(TEXT("读当前未保存内存记录而不是旧结果快照"), Row->VersionTime.Label, FString(TEXT("2026-11-08 05:06:07 UTC")));
    Fixture.Animations[0]->AssetImportData->SourceData.SourceFiles[0].Timestamp = FDateTime(2026, 11, 9, 6, 7, 8);
    Window->RefreshVersionTimes();
    TestEqual(TEXT("再次更新后版本时间可即时刷新"), Row->VersionTime.Label, FString(TEXT("2026-11-09 06:07:08 UTC")));
    TestEqual(TEXT("版本刷新不改原来源记录"), Row->Asset->SourceJson, OriginalJson);
    TestEqual(TEXT("版本刷新不改结果文字"), Row->Status, FString(TEXT("已更新，未保存")));
    Window->ToggleResultColumn(TEXT("VersionTime"));
    TestFalse(TEXT("版本时间可以通过齿轮隐藏"), Window->IsResultColumnVisible(TEXT("VersionTime")));
    Window->ResetResultColumns();
    TestTrue(TEXT("恢复默认会显示版本时间"), Window->IsResultColumnVisible(TEXT("VersionTime")));
    TestEqual(TEXT("版本刷新和显隐不写资产"), Fixture.SaveCount, 0);
    return true;
}
#endif



