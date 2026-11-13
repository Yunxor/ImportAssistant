#include "ImportAssistantAssetAccess.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "ImportAssistantAssetIndex.h"
#include "ImportAssistantAssetState.h"
#include "ImportAssistantReimportQueue.h"
#include "ImportAssistantResultGroups.h"
#include "ImportAssistantSettings.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorReimportHandler.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "SkeletalMeshAttributes.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshModel.h"
#include "StaticMeshResources.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"

using namespace ImportAssistant;

namespace
{
class FMeshTestHandler : public FReimportHandler
{
public:
    TSet<UObject*> Assets;
    int32 Count = 0;
    int32 LastIndex = INDEX_NONE;
    virtual int32 GetPriority() const override { return MAX_int32; }
    virtual bool CanReimport(UObject* Object, TArray<FString>& Files) override
    {
        if (!Assets.Contains(Object)) { return false; }
        GetImportData(Object)->ExtractFilenames(Files); return true;
    }
    virtual void SetReimportPaths(UObject* Object, const TArray<FString>& Files) override
    {
        if (Assets.Contains(Object) && !Files.IsEmpty()) { GetImportData(Object)->UpdateFilenameOnly(Files[0]); }
    }
    virtual void SetReimportPaths(UObject* Object, const FString& File, const int32 Index) override
    {
        if (Assets.Contains(Object)) { GetImportData(Object)->UpdateFilenameOnly(File, Index); }
    }
    virtual EReimportResult::Type Reimport(UObject* Object) override
    {
        ++Count;
        GetImportData(Object)->Update(GetImportData(Object)->GetFirstFilename());
        Object->MarkPackageDirty();
        if (USkeleton* Skeleton = GetRelatedSkeleton(Object)) { Skeleton->MarkPackageDirty(); }
        return EReimportResult::Succeeded;
    }
    virtual EReimportResult::Type Reimport(UObject* Object, int32 Index) override
    {
        LastIndex = Index;
        if (Index == 0) { return Reimport(Object); }
        ++Count;
        Object->MarkPackageDirty();
        if (USkeleton* Skeleton = GetRelatedSkeleton(Object)) { Skeleton->MarkPackageDirty(); }
        return EReimportResult::Succeeded;
    }
};

struct FMeshFixture
{
    FString Root, Source;
    TStrongObjectPtr<UStaticMesh> Static;
    TStrongObjectPtr<USkeletalMesh> Skeletal;
    TStrongObjectPtr<USkeleton> Skeleton;
    TStrongObjectPtr<UPhysicsAsset> Physics;
    TStrongObjectPtr<UMaterial> Material;
    TArray<FRowPtr> Rows;
    FMeshTestHandler Handler;
    int32 Saves = 0;
    FDelegateHandle SaveHandle;

    FMeshFixture()
    {
        const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Root = TEXT("/Game/__ImportAssistantMulti_") + Id;
        const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ImportAssistantTests") / Id);
        IFileManager::Get().MakeDirectory(*Directory, true);
        Source = Directory / TEXT("Mesh.fbx");
        FFileHelper::SaveStringToFile(TEXT("仅供内存处理器测试，不是真实 FBX"), *Source);
        Static.Reset(NewObject<UStaticMesh>(CreatePackage(*(Root / TEXT("Static"))), TEXT("Static"), RF_Public | RF_Standalone));
        Static->SetAssetImportData(NewObject<UFbxStaticMeshImportData>(Static.Get()));
        Static->GetAssetImportData()->SourceData.SourceFiles.Emplace(Source);
        Static->SetNumSourceModels(1);
        FMeshDescription* Description = Static->CreateMeshDescription(0);
        FStaticMeshAttributes Attributes(*Description); Attributes.Register();
        const auto Positions = Attributes.GetVertexPositions();
        TArray<FVertexInstanceID> Corners;
        const TArray<FVector3f> Points = {FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(0, 100, 0)};
        for (const FVector3f& Point : Points)
        {
            const FVertexID Vertex = Description->CreateVertex(); Positions[Vertex] = Point;
            const FVertexInstanceID Corner = Description->CreateVertexInstance(Vertex);
            Attributes.GetVertexInstanceNormals()[Corner] = FVector3f(0, 0, 1);
            Attributes.GetVertexInstanceTangents()[Corner] = FVector3f(1, 0, 0);
            Attributes.GetVertexInstanceBinormalSigns()[Corner] = 1.0f;
            Corners.Add(Corner);
        }
        Description->CreatePolygon(Description->CreatePolygonGroup(), Corners);
        Static->CommitMeshDescription(0);
        Static->Build(false);
        UObject* BuiltObjects[] = {Static.Get()};
        FAssetCompilingManager::Get().FinishCompilationForObjects(BuiltObjects);
        Material.Reset(NewObject<UMaterial>(CreatePackage(*(Root / TEXT("Material"))), TEXT("Material"), RF_Public | RF_Standalone));
        Static->GetStaticMaterials().Add(FStaticMaterial(Material.Get()));
        Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root / TEXT("Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
        {
            FReferenceSkeletonModifier Modifier(Skeleton.Get());
            Modifier.Add(FMeshBoneInfo(TEXT("root"),TEXT("root"),INDEX_NONE), FTransform::Identity);
        }
        Skeletal.Reset(NewObject<USkeletalMesh>(CreatePackage(*(Root / TEXT("Skeletal"))), TEXT("Skeletal"), RF_Public | RF_Standalone));
        Skeletal->SetSkeleton(Skeleton.Get());
        Skeletal->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();
        Skeletal->SetAssetImportData(NewObject<UFbxSkeletalMeshImportData>(Skeletal.Get()));
        Skeletal->GetAssetImportData()->SourceData.SourceFiles.Emplace(Source);
        Skeletal->SetNumSourceModels(1);
        Skeletal->AddLODInfo();
        FMeshDescription Geometry;
        Static->CloneMeshDescription(0, Geometry);
        FSkeletalMeshAttributes SkinAttributes(Geometry); SkinAttributes.Register();
        const FBoneID RootBone = SkinAttributes.CreateBone();
        SkinAttributes.GetBoneNames()[RootBone] = TEXT("root");
        SkinAttributes.GetBoneParentIndices()[RootBone] = INDEX_NONE;
        SkinAttributes.GetBonePoses()[RootBone] = FTransform::Identity;
        for (const FVertexID Vertex : Geometry.Vertices().GetElementIDs())
        {
            SkinAttributes.GetVertexSkinWeights().Set(Vertex, {UE::AnimationCore::FBoneWeight(0, 1.0f)});
        }
        Skeletal->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel());
        Skeletal->CreateMeshDescription(0, MoveTemp(Geometry));
        Skeletal->CommitMeshDescription(0);
        Skeletal->Build();
        UObject* BuiltSkinned[] = {Skeletal.Get()};
        FAssetCompilingManager::Get().FinishCompilationForObjects(BuiltSkinned);
        Physics.Reset(NewObject<UPhysicsAsset>(CreatePackage(*(Root / TEXT("Physics"))), TEXT("Physics"), RF_Public | RF_Standalone));
        Skeletal->SetPhysicsAsset(Physics.Get());
        for (UObject* Object : {static_cast<UObject*>(Static.Get()), static_cast<UObject*>(Skeletal.Get()), static_cast<UObject*>(Skeleton.Get()),
            static_cast<UObject*>(Physics.Get()), static_cast<UObject*>(Material.Get())}) { Object->GetPackage()->SetDirtyFlag(false); }
        for (UObject* Object : {static_cast<UObject*>(Static.Get()), static_cast<UObject*>(Skeletal.Get())})
        {
            FAssetRegistryModule::AssetCreated(Object);
            Handler.Assets.Add(Object);
            FRowPtr Row = MakeShared<FMatchRow>();
            Row->Asset = MakeRecord(FAssetData(Object), TEXT("")); Row->File = Source; Row->SourceIndex = 0; Row->bSelected = true;
            Rows.Add(Row);
        }
        SaveHandle = UPackage::PreSavePackageWithContextEvent.AddLambda([this](UPackage* Package, FObjectPreSaveContext)
            { if (Package->GetName().StartsWith(Root)) { ++Saves; } });
    }
    ~FMeshFixture()
    {
        UPackage::PreSavePackageWithContextEvent.Remove(SaveHandle);
        for (UObject* Object : {static_cast<UObject*>(Static.Get()), static_cast<UObject*>(Skeletal.Get()), static_cast<UObject*>(Skeleton.Get()),
            static_cast<UObject*>(Physics.Get()), static_cast<UObject*>(Material.Get())})
        {
            FAssetRegistryModule::AssetDeleted(Object);
            Object->ClearFlags(RF_Public | RF_Standalone); Object->SetFlags(RF_Transient);
            Object->GetPackage()->SetDirtyFlag(false); Object->GetPackage()->SetFlags(RF_Transient);
        }
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiTypeConfigTest, "ImportAssistant.MultiType.Config", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiTypeConfigTest::RunTest(const FString&)
{
    TStrongObjectPtr<UImportAssistantSettings> Settings(NewObject<UImportAssistantSettings>());
    TestEqual(TEXT("默认三类启用"), Settings->GetAssetClasses().Num(), 3);
    const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("ImportAssistantTests")/FGuid::NewGuid().ToString()/TEXT("Types.ini"));
    Settings->bIncludeAnimations = false; Settings->bIncludeSkeletalMeshes = false;
    FString Error;
    TestTrue(TEXT("类型选择保存"), Settings->SaveToFile(File, Error));
    TStrongObjectPtr<UImportAssistantSettings> Loaded(NewObject<UImportAssistantSettings>());
    TestTrue(TEXT("类型选择重新载入"), Loaded->LoadFromFile(File, Error));
    TestTrue(TEXT("筛选静态模型"), Loaded->GetAssetCategories() == TArray<EImportAssistantAssetCategory>{EImportAssistantAssetCategory::StaticMesh});
    Loaded->bIncludeStaticMeshes = false;
    TArray<FName> Roots; TArray<FPathMapping> Mappings;
    TestFalse(TEXT("不能选择空类型集执行"), Loaded->Parse(Roots, Mappings, Error));
    const FString Legacy = File + TEXT(".legacy.ini");
    FFileHelper::SaveStringToFile(TEXT("[/Script/ImportAssistant.ImportAssistantSettings]\nAssetDirectories=(Path=\"/Game\")\n"), *Legacy);
    TestTrue(TEXT("旧配置仍可读取"), Loaded->LoadFromFile(Legacy, Error));
    TestEqual(TEXT("旧配置缺少类型字段默认启用三类"), Loaded->GetAssetClasses().Num(), 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiTypeTemplatesTest, "ImportAssistant.MultiType.TemplateIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiTypeTemplatesTest::RunTest(const FString&)
{
    TArray<FImportAssistantStorageTemplate> Templates;
    const TArray<EImportAssistantAssetCategory> Categories = {EImportAssistantAssetCategory::Animation,EImportAssistantAssetCategory::StaticMesh,EImportAssistantAssetCategory::SkeletalMesh};
    const TArray<FString> Directories = {TEXT("/Game/Typed/{Name}/Animations"),TEXT("/Game/Typed/{Name}/Static"),TEXT("/Game/Typed/{Name}/Skeletal")};
    for (int32 Index=0; Index<3; ++Index)
    {
        auto& T = Templates.AddDefaulted_GetRef(); T.TemplateName = FString::FromInt(Index); T.AssetCategory = Categories[Index];
        T.DirectoryTemplate = Directories[Index]; auto& Field=T.Fields.AddDefaulted_GetRef(); Field.Position=1; Field.Variable=TEXT("Name");
    }
    const FString File=TEXT("C:/Male_Run.fbx"); TArray<FFileSearchScope> Scopes; TArray<FName> Roots; FString Error;
    TestTrue(TEXT("三类范围计划"), BuildMultiTypeStoragePlan(Templates,true,EImportAssistantStorageSearchMode::TemplateOnly,Categories,{File},
        {FName(TEXT("/Game/Legacy"))},true,Scopes,Roots,Error));
    TestEqual(TEXT("同一 FBX 按三类独立建范围"),Scopes.Num(),3);
    FSearchSnapshot Index;
    for (int32 Type=0;Type<3;++Type)
    {
        for (int32 Folder=0;Folder<3;++Folder)
        {
            auto Record=MakeShared<FAssetRecord,ESPMode::ThreadSafe>();
            Record->AssetClassPath=GetAssetClassPath(Categories[Type]); Record->PackageName=Directories[Folder].Replace(TEXT("{Name}"),TEXT("Male"))/FString::Printf(TEXT("Run%d%d"),Type,Folder);
            Record->Name=FPaths::GetCleanFilename(Record->PackageName); Record->ObjectPath=FSoftObjectPath(Record->PackageName+TEXT(".")+Record->Name);
            Record->Sources.Add({File,{NormalizePath(File)}}); Index.Records.Add(Record); Index.BySource.FindOrAdd(NormalizePath(File)).Add(Record);
        }
    }
    std::atomic_bool Cancel(false); FMatchSettings Settings;
    auto Result=Match(Index,{File},Settings,{},Cancel,Scopes);
    TestEqual(TEXT("目录合并不导致跨类别误入"),Result.Rows.Num(),3);
    const auto Groups=BuildResultGroups({File},Result.Rows,false,Scopes);
    TestTrue(TEXT("分组说明保留所有类别"),Groups[0]->ScopeDetails.Contains(TEXT("静态模型"))&&Groups[0]->ScopeDetails.Contains(TEXT("骨骼模型")));
    Settings.bMatchKeyword=true; Settings.bMatchPath=false; Settings.bAllKeywords=false;
    TestEqual(TEXT("关键词也遵守类型目录隔离"),Match(Index,{File},Settings,{},Cancel,Scopes).Rows.Num(),3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiTypeDirtyTest,"ImportAssistant.MultiType.DirtyDependencies",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiTypeDirtyTest::RunTest(const FString&)
{
    FMeshFixture Fixture;
    TestTrue(TEXT("静态模型不依赖 Skeleton"),InspectDirtyAssets(*Fixture.Rows[0]->Asset).bKnown && GetRelatedSkeleton(Fixture.Static.Get())==nullptr);
    Fixture.Static->MarkPackageDirty(); Fixture.Material->MarkPackageDirty();
    auto StaticInfo=InspectDirtyAssets(*Fixture.Rows[0]->Asset);
    TestTrue(TEXT("静态模型及直接材质脏修改可见"),StaticInfo.bAssetDirty && StaticInfo.DirtyDependencies.Contains(Fixture.Material->GetPathName()));
    Fixture.Skeleton->MarkPackageDirty(); Fixture.Physics->MarkPackageDirty();
    auto SkeletalInfo=InspectDirtyAssets(*Fixture.Rows[1]->Asset);
    TestTrue(TEXT("骨骼模型关联骨骼及物理资产可见"),SkeletalInfo.bSkeletonDirty && SkeletalInfo.DirtyDependencies.Contains(Fixture.Physics->GetPathName()));
    const auto Plan=BuildReimportPlan(Fixture.Rows);
    TestTrue(TEXT("模型计划不再强制转换动画"),Plan->Error.IsEmpty());
    TestTrue(TEXT("覆盖风险列出关联物理和材质"),BuildReimportWarning(*Plan).Contains(Fixture.Physics->GetPathName())&&BuildReimportWarning(*Plan).Contains(Fixture.Material->GetPathName()));
    FString Error; Fixture.Material->MarkPackageDirty();
    TestFalse(TEXT("确认后直接关联依赖新修改使许可过期"),IsReimportPlanCurrent(*Plan,Error));
    TestEqual(TEXT("状态检查不保存"),Fixture.Saves,0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiTypeSlotTest,"ImportAssistant.MultiType.SourceSlotPolicy",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiTypeSlotTest::RunTest(const FString&)
{
    FMeshFixture Fixture; FString Error;
    TestTrue(TEXT("静态模型主槽位支持"),ValidateReimportSupport(Fixture.Static.Get(),0,Error));
    Fixture.Static->GetAssetImportData()->SourceData.SourceFiles.Emplace(Fixture.Source);
    TestFalse(TEXT("静态模型拒绝非主槽位"),ValidateReimportSupport(Fixture.Static.Get(),1,Error));
    for(int32 Index=1;Index<3;++Index){ Fixture.Skeletal->GetAssetImportData()->SourceData.SourceFiles.Emplace(Fixture.Source); }
    TestTrue(TEXT("骨骼几何槽位通过基础几何校验"),ValidateReimportSupport(Fixture.Skeletal.Get(),1,Error));
    TestTrue(TEXT("骨骼蒙皮槽位支持"),ValidateReimportSupport(Fixture.Skeletal.Get(),2,Error));
    Fixture.Skeletal->GetImportedModel()->LODModels.Empty();
    TestFalse(TEXT("缺几何时禁止静默转完整导入"),ValidateReimportSupport(Fixture.Skeletal.Get(),1,Error));
    Fixture.Static->GetAssetImportData()->SetFlags(RF_Transient);
    CastChecked<UFbxStaticMeshImportData>(Fixture.Static->GetAssetImportData())->bImportAsScene=true;
    TestFalse(TEXT("拒绝场景导入的普通更新"),ValidateReimportSupport(Fixture.Static.Get(),0,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiTypeQueueTest,"ImportAssistant.MultiType.MeshQueueNoSave",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiTypeQueueTest::RunTest(const FString&)
{
    FMeshFixture Fixture;
    TestTrue(TEXT("静态模型测试几何有效"),HasValidImportedData(Fixture.Static.Get()));
    TestTrue(TEXT("骨骼模型测试几何有效"),HasValidImportedData(Fixture.Skeletal.Get()));
    const auto Plan=BuildReimportPlan(Fixture.Rows);
    if(!TestTrue(TEXT("模型预检可完成"),Plan->Error.IsEmpty())){AddError(Plan->Error); return false;}
    FReimportQueue Queue; Queue.Start(Fixture.Rows,Plan);
    for(int32 Step=0;Step<9&&Queue.IsRunning();++Step){Queue.Tick();}
    TestEqual(TEXT("两种模型均通过真实队列执行"),Fixture.Handler.Count,2);
    for (const auto& Row : Fixture.Rows)
    {
        TestTrue(TEXT("更新保持待保存结果：") + Row->Asset->AssetClassPath.ToString() + TEXT("；") + Row->Status, Row->Status.StartsWith(TEXT("已更新，未保存")));
    }
    TestTrue(TEXT("两模型脏标记保持"),Fixture.Static->GetPackage()->IsDirty()&&Fixture.Skeletal->GetPackage()->IsDirty());
    const auto Next=BuildReimportPlan(Fixture.Rows,true); Queue.Start(Fixture.Rows,Next);
    for(int32 Step=0;Step<9&&Queue.IsRunning();++Step){Queue.Tick();}
    TestEqual(TEXT("模型支持连续新批次"),Fixture.Handler.Count,4);
    const FString Replacement = Fixture.Source + TEXT(".replacement.fbx");
    FFileHelper::SaveStringToFile(TEXT("仅供模型处理器测试"), *Replacement);
    Fixture.Rows[0]->File = Replacement;
    Fixture.Rows[1]->bSelected = false;
    const auto Rebind = BuildReimportPlan(Fixture.Rows, true);
    TestTrue(TEXT("模型来源改绑产生计划风险"), BuildReimportWarning(*Rebind).Contains(TEXT("来源改绑")));
    Queue.Start(Fixture.Rows, Rebind);
    for (int32 Step = 0; Step < 6 && Queue.IsRunning(); ++Step) { Queue.Tick(); }
    TestEqual(TEXT("模型实际改绑后的来源"), NormalizePath(GetImportData(Fixture.Static.Get())->GetFirstFilename()), NormalizePath(Replacement));
    TestEqual(TEXT("全部模型路径零保存调用"),Fixture.Saves,0);
    return true;
}

class FMultiTypeIndexCommand : public IAutomationLatentCommand
{
public:
    explicit FMultiTypeIndexCommand(FAutomationTestBase* InTest):Test(InTest),Start(FPlatformTime::Seconds())
    {
        Index.Configure({FName(Fixture.Root)},true,{GetAssetClassPath(EImportAssistantAssetCategory::StaticMesh)});
    }
    virtual bool Update() override
    {
        Index.Tick(); if(!Index.IsReady()){if(FPlatformTime::Seconds()-Start>30){Test->AddError(TEXT("模型索引超时"));return true;} return false;}
        const auto Snapshot=Index.Snapshot();
        if(Phase==0)
        {
            Test->TestEqual(TEXT("类型筛选只包含静态模型"),Snapshot->Records.Num(),1);
            Fixture.Static->GetAssetImportData()->UpdateFilenameOnly(Fixture.Source+TEXT(".new")); Fixture.Static->MarkPackageDirty(); ++Phase;return false;
        }
        if(Phase==1)
        {
            Test->TestTrue(TEXT("模型来源修改增量同步"),Snapshot->Records.Num()==1&&Snapshot->Records[0]->Sources[0].Original.Contains(TEXT(".new")));
            Index.Configure({FName(Fixture.Root)},true);++Phase;return false;
        }
        Test->TestEqual(TEXT("全部类型包含两种内存模型"),Snapshot->Records.Num(),2);
        return true;
    }
private:
    FAutomationTestBase* Test;double Start;int32 Phase=0;FMeshFixture Fixture;FAssetIndex Index;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiTypeIndexTest,"ImportAssistant.MultiType.MemoryIndex",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiTypeIndexTest::RunTest(const FString&)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FMultiTypeIndexCommand>(this));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiTypeMixedTest, "ImportAssistant.MultiType.MixedBatch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiTypeMixedTest::RunTest(const FString&)
{
    FMeshFixture Fixture;
    TStrongObjectPtr<UAnimSequence> Animation(NewObject<UAnimSequence>(CreatePackage(*(Fixture.Root / TEXT("Animation"))), TEXT("Animation"), RF_Public | RF_Standalone));
    Animation->SetSkeleton(Fixture.Skeleton.Get());
    auto& Controller = Animation->GetController();
    Controller.InitializeModel(); Controller.SetFrameRate(FFrameRate(30, 1), false); Controller.SetNumberOfFrames(1, false); Controller.NotifyPopulated();
    UObject* Compiling[] = {Animation.Get()}; FAssetCompilingManager::Get().FinishCompilationForObjects(Compiling);
    Animation->AssetImportData = NewObject<UFbxAnimSequenceImportData>(Animation.Get());
    Animation->AssetImportData->SourceData.SourceFiles.Emplace(Fixture.Source);
    Animation->GetPackage()->SetDirtyFlag(false); Fixture.Skeleton->GetPackage()->SetDirtyFlag(false);
    FRowPtr Row = MakeShared<FMatchRow>(); Row->Asset = MakeRecord(FAssetData(Animation.Get()), TEXT(""));
    Row->File = Fixture.Source; Row->SourceIndex = 0; Row->bSelected = true;
    Fixture.Rows.Add(Row); Fixture.Handler.Assets.Add(Animation.Get());
    {
        const auto Plan = BuildReimportPlan(Fixture.Rows);
        TestTrue(TEXT("三种混合对象预检通过"), Plan->Error.IsEmpty());
        FReimportQueue Queue; Queue.Start(Fixture.Rows, Plan);
        for (int32 Step = 0; Step < 12 && Queue.IsRunning(); ++Step) { Queue.Tick(); }
        TestEqual(TEXT("共享 Skeleton 的动画与模型批次都执行"), Fixture.Handler.Count, 3);
        for (const auto& Item : Fixture.Rows) { TestTrue(TEXT("混合类型结果正确"), Item->Status.StartsWith(TEXT("已更新，未保存"))); }
        TestEqual(TEXT("混合批次没有自动保存"), Fixture.Saves, 0);
    }
    FAssetRegistryModule::AssetDeleted(Animation.Get());
    Animation->ClearFlags(RF_Public | RF_Standalone); Animation->SetFlags(RF_Transient);
    Animation->GetPackage()->SetDirtyFlag(false); Animation->GetPackage()->SetFlags(RF_Transient);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiTypeSparseSourceTest, "ImportAssistant.MultiType.SparseSourceSlots", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiTypeSparseSourceTest::RunTest(const FString&)
{
    FMeshFixture Fixture;
    auto* Data = Fixture.Skeletal->GetAssetImportData();
    Data->SourceData.SourceFiles.Emplace(TEXT(""));
    Data->SourceData.SourceFiles.Emplace(Fixture.Source, FDateTime(2026, 11, 10), FMD5Hash(), TEXT("蒙皮权重"));
    const auto Record = MakeRecord(FAssetData(Fixture.Skeletal.Get()), TEXT(""));
    TestEqual(TEXT("原生 JSON 空槽位不丢失"), Record->Sources.Num(), 3);
    TestTrue(TEXT("空几何槽位仍为空"), Record->Sources[1].Original.IsEmpty());
    TestEqual(TEXT("蒙皮标签保留"), Record->Sources[2].DisplayLabel, FString(TEXT("蒙皮权重")));
    FRowPtr Row = MakeShared<FMatchRow>(); Row->Asset = Record; Row->SourceIndex = 2; Row->File = Fixture.Source; Row->bSelected = true;
    const auto Plan = BuildReimportPlan({Row});
    TestTrue(TEXT("蒙皮更新预检通过"), Plan->Error.IsEmpty());
    TestTrue(TEXT("非主槽位即使干净同源仍提示具体内容"), BuildReimportWarning(*Plan).Contains(TEXT("仅蒙皮权重")));
    FReimportQueue Queue; Queue.Start({Row}, Plan);
    for (int32 Step = 0; Step < 6 && Queue.IsRunning(); ++Step) { Queue.Tick(); }
    TestEqual(TEXT("传给重导入后端的确实是槽位2"), Fixture.Handler.LastIndex, 2);
    TestTrue(TEXT("分项内容更新完成且未保存"), Row->Status.StartsWith(TEXT("已更新，未保存")));
    TestEqual(TEXT("槽位更新不保存"), Fixture.Saves, 0);
    FSearchSnapshot Snapshot; Snapshot.Records.Add(Record);
    for (const auto& Source : Record->Sources)
    {
        for (const auto& Key : Source.Candidates) { Snapshot.BySource.FindOrAdd(Key).AddUnique(Record); }
    }
    std::atomic_bool Cancel(false); FMatchSettings Settings;
    const auto Matched = Match(Snapshot, {Fixture.Source}, Settings, {}, Cancel);
    TestTrue(TEXT("完整和蒙皮同文件时仍标槽位歧义，不自行选错更新内容"), Matched.Rows.Num() == 1 && Matched.Rows[0]->bSourceAmbiguous);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultiTypeUnknownTest, "ImportAssistant.MultiType.ScopeFallbackAndUnknown", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMultiTypeUnknownTest::RunTest(const FString&)
{
    FImportAssistantStorageTemplate Template;
    Template.TemplateName = TEXT("仅静态"); Template.AssetCategory = EImportAssistantAssetCategory::StaticMesh;
    Template.DirectoryTemplate = TEXT("/Game/StaticOnly");
    TArray<FFileSearchScope> Scopes; TArray<FName> Roots; FString Error;
    const FString File = TEXT("C:/Mesh.fbx");
    BuildMultiTypeStoragePlan({Template}, true, EImportAssistantStorageSearchMode::TemplateOnly,
        {EImportAssistantAssetCategory::StaticMesh, EImportAssistantAssetCategory::SkeletalMesh}, {File}, {FName(TEXT("/Game/Fallback"))}, true, Scopes, Roots, Error);
    TestTrue(TEXT("无骨骼模板按类型单独回退"), Scopes.ContainsByPredicate([](const FFileSearchScope& Scope)
        { return Scope.AssetClassPath == GetAssetClassPath(EImportAssistantAssetCategory::SkeletalMesh) && Scope.SearchRoots.Contains(TEXT("/Game/Fallback")); }));
    FSearchSnapshot Index;
    for (auto Category : {EImportAssistantAssetCategory::Animation, EImportAssistantAssetCategory::StaticMesh, EImportAssistantAssetCategory::SkeletalMesh})
    {
        auto Record = MakeShared<FAssetRecord, ESPMode::ThreadSafe>(); Record->AssetClassPath = GetAssetClassPath(Category);
        Record->PackageName = TEXT("/Game/StaticOnly/") + Record->AssetClassPath.GetAssetName().ToString();
        Record->ObjectPath = FSoftObjectPath(Record->PackageName + TEXT(".") + Record->AssetClassPath.GetAssetName().ToString());
        Index.UnknownRecords.Add(Record);
    }
    std::atomic_bool Cancel(false); FMatchSettings Settings;
    const auto Result = Match(Index, {File}, Settings, {}, Cancel, Scopes);
    TestEqual(TEXT("未知来源诊断也按类型和目录隔离，不把动画/骨骼串进静态目录"), Result.UnmatchedUnknownCount, 1);
    TestTrue(TEXT("模板范围内的未知来源不自动成为匹配结果"), Result.Rows.IsEmpty());
    return true;
}
#endif
