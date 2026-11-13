#include "ImportAssistantAssetAccess.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "InterchangeAssetImportData.h"
#include "InterchangeManager.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshModel.h"

namespace ImportAssistant
{
FTopLevelAssetPath GetAssetClassPath(EImportAssistantAssetCategory Category)
{
    switch (Category)
    {
    case EImportAssistantAssetCategory::StaticMesh: return UStaticMesh::StaticClass()->GetClassPathName();
    case EImportAssistantAssetCategory::SkeletalMesh: return USkeletalMesh::StaticClass()->GetClassPathName();
    default: return UAnimSequence::StaticClass()->GetClassPathName();
    }
}

TArray<FTopLevelAssetPath> GetSupportedAssetClasses()
{
    return {UAnimSequence::StaticClass()->GetClassPathName(), UStaticMesh::StaticClass()->GetClassPathName(), USkeletalMesh::StaticClass()->GetClassPathName()};
}

bool IsSupportedFbxAsset(const UObject* Object)
{
    return Object && (Object->IsA<UAnimSequence>() || Object->IsA<UStaticMesh>() || Object->IsA<USkeletalMesh>());
}

FString GetAssetTypeLabel(const FTopLevelAssetPath& ClassPath)
{
    if (ClassPath == UStaticMesh::StaticClass()->GetClassPathName()) { return TEXT("静态模型"); }
    if (ClassPath == USkeletalMesh::StaticClass()->GetClassPathName()) { return TEXT("骨骼模型"); }
    return ClassPath == UAnimSequence::StaticClass()->GetClassPathName() ? TEXT("动画") : TEXT("资产");
}

UAssetImportData* GetImportData(UObject* Object)
{
    if (auto* Animation = Cast<UAnimSequence>(Object)) { return Animation->AssetImportData; }
    if (auto* Mesh = Cast<UStaticMesh>(Object)) { return Mesh->GetAssetImportData(); }
    if (auto* Mesh = Cast<USkeletalMesh>(Object)) { return Mesh->GetAssetImportData(); }
    return nullptr;
}

USkeleton* GetRelatedSkeleton(UObject* Object)
{
    if (auto* Animation = Cast<UAnimSequence>(Object)) { return Animation->GetSkeleton(); }
    if (auto* Mesh = Cast<USkeletalMesh>(Object)) { return Mesh->GetSkeleton(); }
    return nullptr;
}

TArray<UObject*> GetDirectReimportDependencies(UObject* Object)
{
    TArray<UObject*> Result;
    if (USkeleton* Skeleton = GetRelatedSkeleton(Object)) { Result.Add(Skeleton); }
    // 仅检查已直接引用的潜在导入依赖，不遍历全工程引用图或递归加载依赖。
    if (auto* Mesh = Cast<UStaticMesh>(Object))
    {
        if (UObject* Body = Mesh->GetBodySetup()) { Result.AddUnique(Body); }
        for (const FStaticMaterial& Material : Mesh->GetStaticMaterials())
        {
            if (Material.MaterialInterface) { Result.AddUnique(Material.MaterialInterface); }
        }
    }
    if (auto* Mesh = Cast<USkeletalMesh>(Object))
    {
        if (UObject* Physics = Mesh->GetPhysicsAsset()) { Result.AddUnique(Physics); }
        for (const FSkeletalMaterial& Material : Mesh->GetMaterials())
        {
            if (Material.MaterialInterface) { Result.AddUnique(Material.MaterialInterface); }
        }
    }
    return Result;
}

FString DescribeSourceSlot(const FTopLevelAssetPath& ClassPath, int32 SourceIndex)
{
    if (SourceIndex == INDEX_NONE) { return TEXT("来源槽位未确定"); }
    if (ClassPath == USkeletalMesh::StaticClass()->GetClassPathName())
    {
        return SourceIndex < 3 ? FString::Printf(TEXT("来源槽位 %d（%s；Legacy 语义，非 LOD）"), SourceIndex,
            SourceIndex == 0 ? TEXT("完整内容") : SourceIndex == 1 ? TEXT("仅几何体") : TEXT("仅蒙皮权重")) :
            FString::Printf(TEXT("来源槽位 %d（需后端确认）"), SourceIndex);
    }
    return FString::Printf(TEXT("来源槽位 %d"), SourceIndex);
}

bool ValidateReimportSupport(UObject* Object, int32 SourceIndex, FString& Error)
{
    UAssetImportData* Data = GetImportData(Object);
    if (!IsSupportedFbxAsset(Object) || !Data) { Error = TEXT("不支持的资产类型或缺少导入数据"); return false; }
    if (!Data->GetSourceData().SourceFiles.IsValidIndex(SourceIndex)) { Error = TEXT("来源槽位不存在"); return false; }
    if (!Object->IsA<UStaticMesh>() && !GetRelatedSkeleton(Object)) { Error = TEXT("缺少关联 Skeleton"); return false; }
    if (auto* Mesh = Cast<USkeletalMesh>(Object); Mesh && Mesh->HasCustomActorReimportFactory())
    {
        Error = TEXT("自定义角色重导入工厂需使用原流程"); return false;
    }
    if (Data->IsA<UInterchangeAssetImportData>())
    {
        const auto Type = Object->IsA<UAnimSequence>() ? EInterchangeTranslatorAssetType::Animations : EInterchangeTranslatorAssetType::Meshes;
        const auto Formats = UInterchangeManager::GetInterchangeManager().GetSupportedAssetTypeFormats(Type);
        if (!UInterchangeManager::IsInterchangeImportEnabled() || !Formats.ContainsByPredicate([](const FString& Format)
            { return Format.StartsWith(TEXT("fbx;"), ESearchCase::IgnoreCase); }))
        {
            Error = TEXT("当前 Interchange FBX 后端未启用，不自动转换旧导入器"); return false;
        }
        return true;
    }
    const bool Correct = (Object->IsA<UAnimSequence>() && Data->IsA<UFbxAnimSequenceImportData>()) ||
        (Object->IsA<UStaticMesh>() && Data->IsA<UFbxStaticMeshImportData>()) || (Object->IsA<USkeletalMesh>() && Data->IsA<UFbxSkeletalMeshImportData>());
    if (!Correct) { Error = TEXT("资产与 Legacy FBX 导入数据类型不匹配，不自动转换"); return false; }
    if (CastChecked<UFbxAssetImportData>(Data)->bImportAsScene) { Error = TEXT("场景导入资产不支持普通资产更新，请使用场景重导入"); return false; }
    if (auto* Mesh = Cast<USkeletalMesh>(Object))
    {
        if (SourceIndex < 0 || SourceIndex > 2) { Error = TEXT("Legacy 骨骼模型仅支持完整内容/几何体/蒙皮权重三个来源槽位"); return false; }
        if (SourceIndex > 0 && (!Mesh->IsValidLODIndex(0) || !Mesh->HasMeshDescription(0) || !Mesh->GetImportedModel() || Mesh->GetImportedModel()->LODModels.IsEmpty()))
        {
            Error = TEXT("模型没有可单独更新内容的基础几何，禁止将几何/蒙皮槽位误作完整导入"); return false;
        }
    }
    else if (SourceIndex != 0) { Error = TEXT("Legacy 动画或静态模型仅支持主来源槽位，不执行独立 LOD 更新"); return false; }
    return true;
}

bool HasValidImportedData(UObject* Object)
{
    if (auto* Animation = Cast<UAnimSequence>(Object)) { return Animation->GetNumberOfSampledKeys() > 0; }
    if (auto* Mesh = Cast<UStaticMesh>(Object)) { return Mesh->GetNumLODs() > 0 && (Mesh->GetNumVertices(0) > 0 || Mesh->GetNumNaniteVertices() > 0); }
    if (auto* Mesh = Cast<USkeletalMesh>(Object))
    {
        const auto* Model = Mesh->GetImportedModel();
        return Mesh->GetSkeleton() && Mesh->GetRefSkeleton().GetNum() > 0 && Model && !Model->LODModels.IsEmpty() && Model->LODModels[0].NumVertices > 0;
    }
    return false;
}
}
