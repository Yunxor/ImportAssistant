#include "ImportAssistantAssetPresentation.h"

#include "AssetDefinition.h"
#include "AssetDefinitionRegistry.h"
#include "Misc/Paths.h"

namespace ImportAssistant
{
const FAssetTypePresentation& FAssetPresentationCache::Get(const FTopLevelAssetPath& ClassPath)
{
    check(IsInGameThread());
    UAssetDefinitionRegistry* Registry = UAssetDefinitionRegistry::Get();
    const uint64 CurrentVersion = Registry->GetAssetDefinitionVersion();
    if (CurrentVersion != DefinitionVersion)
    {
        Types.Reset();
        DefinitionVersion = CurrentVersion;
    }
    if (const FAssetTypePresentation* Existing = Types.Find(ClassPath)) { return *Existing; }
    FAssetTypePresentation Value;
    Value.ClassName = ClassPath.GetAssetName().ToString();
    Value.DisplayName = FText::FromString(ClassPath.IsNull() ? TEXT("未知类型") : Value.ClassName);
    // 只查找已经注册的类型类和定义，不加载匹配资产，也不通过软路径加载类。
    if (const UClass* Class = FindObject<UClass>(nullptr, *ClassPath.ToString()))
    {
        if (const UAssetDefinition* Definition = Registry->GetAssetDefinitionForClass(Class))
        {
            Value.DisplayName = Definition->GetAssetDisplayName();
            Value.Color = Definition->GetAssetColor();
        }
    }
    Value.Label = FText::FromString(Value.DisplayName.ToString());
    return Types.Add(ClassPath, MoveTemp(Value));
}

FText DescribeAssetFileTypes(const FAssetRecord& Asset, const FAssetTypePresentation& Presentation)
{
    TArray<FString> Extensions;
    for (const FSourceRecord& Source : Asset.Sources)
    {
        const FString Extension = FPaths::GetExtension(Source.Original).ToLower();
        if (!Extension.IsEmpty()) { Extensions.AddUnique(Extension.ToUpper() + TEXT("（.") + Extension + TEXT("）")); }
    }
    return FText::FromString(FString::Printf(TEXT("资产类型：%s\nUE 类：%s\n资产文件格式：.uasset\n源文件类型：%s"),
        *Presentation.DisplayName.ToString(), *Asset.AssetClassPath.ToString(),
        Extensions.IsEmpty() ? TEXT("未记录源文件类型") : *FString::Join(Extensions, TEXT("、"))));
}
}
