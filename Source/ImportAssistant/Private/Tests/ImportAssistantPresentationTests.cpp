#include "ImportAssistantAssetPresentation.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Animation/AnimSequence.h"
#include "AssetDefinition.h"
#include "AssetDefinitionRegistry.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"

using namespace ImportAssistant;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantTypePresentationTest, "ImportAssistant.Editor.OfficialAssetTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantTypePresentationTest::RunTest(const FString&)
{
    FAssetPresentationCache Cache;
    for (const UClass* Class : {UAnimSequence::StaticClass(), UStaticMesh::StaticClass(), USkeletalMesh::StaticClass()})
    {
        const UAssetDefinition* Definition = UAssetDefinitionRegistry::Get()->GetAssetDefinitionForClass(Class);
        if (!TestNotNull(TEXT("官方资产类型定义已注册：") + Class->GetName(), Definition)) { return false; }
        const FAssetTypePresentation& Presentation = Cache.Get(Class->GetClassPathName());
        TestTrue(TEXT("配色来自引擎官方定义：") + Class->GetName(), Presentation.Color.Equals(Definition->GetAssetColor()));
        TestEqual(TEXT("名称来自引擎官方定义"), Presentation.DisplayName.ToString(), Definition->GetAssetDisplayName().ToString());
        TestEqual(TEXT("类型标签沿用简洁的官方名称显示"), Presentation.Label.ToString(), Definition->GetAssetDisplayName().ToString());
        TestEqual(TEXT("完整类名仍由类型记录保留供 Tooltip 使用"), Presentation.ClassName, Class->GetName());
        TestTrue(TEXT("同类重复显示复用缓存"), &Cache.Get(Class->GetClassPathName()) == &Presentation);
    }
    const FAssetTypePresentation& Animation = Cache.Get(UAnimSequence::StaticClass()->GetClassPathName());
    TestTrue(TEXT("动画使用 UE 官方绿色"), Animation.Color.Equals(FLinearColor(FColor(80, 123, 72))));
    FAssetRecord Asset;
    Asset.AssetClassPath = UAnimSequence::StaticClass()->GetClassPathName();
    Asset.Sources = {{TEXT("D:/Art/Run.FBX"), {}}, {TEXT("D:/Backup/Run.fbx"), {}}};
    const FString Tooltip = DescribeAssetFileTypes(Asset, Animation).ToString();
    TestTrue(TEXT("区分 UE 类与 uasset 文件格式"), Tooltip.Contains(TEXT("/Script/Engine.AnimSequence")) && Tooltip.Contains(TEXT("资产文件格式：.uasset")));
    TestTrue(TEXT("源扩展名按元数据获取且去重"), Tooltip.EndsWith(TEXT("源文件类型：FBX（.fbx）")));
    Asset.Sources = {{TEXT("D:/Art/Run.bvh"), {}}};
    TestTrue(TEXT("不把其他源格式强制显示成 FBX"), DescribeAssetFileTypes(Asset, Animation).ToString().Contains(TEXT("BVH（.bvh）")));
    Asset.Sources.Reset();
    TestTrue(TEXT("未知来源不虚构扩展名"), DescribeAssetFileTypes(Asset, Animation).ToString().Contains(TEXT("未记录源文件类型")));
    const FAssetTypePresentation& Unknown = Cache.Get(FTopLevelAssetPath(TEXT("/Script/Unknown.MissingAssetType")));
    TestEqual(TEXT("未注册类型保留类名"), Unknown.ClassName, FString(TEXT("MissingAssetType")));
    TestTrue(TEXT("未注册类型使用中性色"), Unknown.Color.Equals(FLinearColor::Gray));
    return true;
}
#endif

