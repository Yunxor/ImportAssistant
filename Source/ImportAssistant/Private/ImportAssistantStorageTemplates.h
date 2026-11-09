#pragma once

#include "CoreMinimal.h"
#include "ImportAssistantTypes.h"
#include "ImportAssistantStorageTemplates.generated.h"

UENUM()
enum class EImportAssistantAssetCategory : uint8
{
    Animation UMETA(DisplayName = "动画序列"),
    SkeletalMesh UMETA(DisplayName = "骨骼模型（预留）"),
    StaticMesh UMETA(DisplayName = "静态模型（预留）")
};

UENUM()
enum class EImportAssistantStorageSearchMode : uint8
{
    PreferTemplate UMETA(DisplayName = "模板目录优先，补查原资产目录"),
    TemplateOnly UMETA(DisplayName = "仅模板目录；未匹配模板时回退原目录")
};

USTRUCT()
struct FImportAssistantNameField
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "字段", meta = (DisplayName = "词段位置", ClampMin = "1", ToolTip = "移除模板前缀后从 1 开始计数，空词段占据位置"))
    int32 Position = 1;

    UPROPERTY(EditAnywhere, Category = "字段", meta = (DisplayName = "变量名"))
    FString Variable = TEXT("Name");

    UPROPERTY(EditAnywhere, Category = "字段", meta = (DisplayName = "含义说明"))
    FString Description;
};

USTRUCT()
struct FImportAssistantStorageTemplate
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "模板", meta = (DisplayName = "启用"))
    bool bEnabled = true;

    UPROPERTY(EditAnywhere, Category = "模板", meta = (DisplayName = "模板名称"))
    FString TemplateName;

    UPROPERTY(EditAnywhere, Category = "模板", meta = (DisplayName = "资产类别"))
    EImportAssistantAssetCategory AssetCategory = EImportAssistantAssetCategory::Animation;

    UPROPERTY(EditAnywhere, Category = "模板", meta = (DisplayName = "规范说明", MultiLine = true))
    FString Description;

    UPROPERTY(EditAnywhere, Category = "模板", meta = (DisplayName = "名称前缀", ToolTip = "非空时要求 FBX 名称具有此前缀，并在分段前移除。独立于关键词忽略前后缀。"))
    FString NamePrefix;

    UPROPERTY(EditAnywhere, Category = "模板", meta = (DisplayName = "分隔符", ToolTip = "按字面拆分而非正则表达式，例如 _；保留空词段"))
    FString Delimiter = TEXT("_");

    UPROPERTY(EditAnywhere, Category = "模板", meta = (DisplayName = "字段定义", TitleProperty = "Variable"))
    TArray<FImportAssistantNameField> Fields;

    UPROPERTY(EditAnywhere, Category = "模板", meta = (DisplayName = "目录模板", ToolTip = "UE 包目录，例如 /Game/Character/{Name}/Animations；变量名区分大小写，根目录不能使用变量"))
    FString DirectoryTemplate;
};

namespace ImportAssistant
{
bool IsDirectoryInRoots(const FString& Directory, const TArray<FString>& Roots, bool bRecursive);
void CompactStorageRoots(TArray<FString>& Roots, bool bRecursive);
bool BuildStorageSearchPlan(const TArray<FImportAssistantStorageTemplate>& Templates, bool bEnabled,
    EImportAssistantStorageSearchMode Mode, EImportAssistantAssetCategory Category, const TArray<FString>& Files,
    const TArray<FName>& BaseRoots, bool bRecursive, TArray<FFileSearchScope>& OutScopes, TArray<FName>& OutIndexRoots, FString& OutError);
}
