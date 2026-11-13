#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "ImportAssistantTypes.h"
#include "ImportAssistantStorageTemplates.h"
#include "UObject/Object.h"
#include "ImportAssistantSettings.generated.h"

UCLASS(Config = EditorPerProjectUserSettings)
class UImportAssistantSettings : public UObject
{
    GENERATED_BODY()

public:
    UImportAssistantSettings();

    UPROPERTY(EditAnywhere, Config, Category = "检索范围", meta = (DisplayName = "资产目录", LongPackageName, ForceShowPluginContent))
    TArray<FDirectoryPath> AssetDirectories;

    UPROPERTY(EditAnywhere, Config, Category = "检索范围|资产类型", meta = (DisplayName = "动画序列"))
    bool bIncludeAnimations = true;

    UPROPERTY(EditAnywhere, Config, Category = "检索范围|资产类型", meta = (DisplayName = "静态模型"))
    bool bIncludeStaticMeshes = true;

    UPROPERTY(EditAnywhere, Config, Category = "检索范围|资产类型", meta = (DisplayName = "骨骼模型"))
    bool bIncludeSkeletalMeshes = true;

    UPROPERTY(EditAnywhere, Config, Category = "检索范围", meta = (DisplayName = "递归子目录"))
    bool bRecursive = true;

    UPROPERTY(EditAnywhere, Config, Category = "匹配规则", meta = (DisplayName = "源路径匹配"))
    bool bMatchPath = true;

    UPROPERTY(EditAnywhere, Config, Category = "匹配规则", meta = (DisplayName = "名称关键词匹配"))
    bool bMatchKeyword = false;

    UPROPERTY(EditAnywhere, Config, Category = "匹配规则", meta = (DisplayName = "全部关键词", ToolTip = "关闭时匹配任意关键词"))
    bool bAllKeywords = true;

    UPROPERTY(EditAnywhere, Config, Category = "匹配规则", meta = (DisplayName = "忽略前缀", ToolTip = "按数组顺序移除第一个匹配的前缀"))
    TArray<FString> IgnorePrefixes;

    UPROPERTY(EditAnywhere, Config, Category = "匹配规则", meta = (DisplayName = "忽略后缀", ToolTip = "按数组顺序移除第一个匹配的后缀"))
    TArray<FString> IgnoreSuffixes;

    UPROPERTY(EditAnywhere, Config, Category = "路径映射", meta = (DisplayName = "源目录映射", MultiLine = true,
        ToolTip = "每行一条：D:/OldArt => E:/Art。只用于检索，更新前单独确认来源改绑。"))
    FString MappingsText;

    UPROPERTY(EditAnywhere, Config, Category = "资产存放模板", meta = (DisplayName = "启用资产存放模板"))
    bool bUseStorageTemplates = false;

    UPROPERTY(EditAnywhere, Config, Category = "资产存放模板", meta = (DisplayName = "检索策略", EditCondition = "bUseStorageTemplates"))
    EImportAssistantStorageSearchMode StorageSearchMode = EImportAssistantStorageSearchMode::PreferTemplate;

    UPROPERTY(EditAnywhere, Config, Category = "资产存放模板", meta = (DisplayName = "存放模板", TitleProperty = "TemplateName"))
    TArray<FImportAssistantStorageTemplate> StorageTemplates;

    UPROPERTY(Config)
    TArray<FName> HiddenResultColumns;

    TArray<EImportAssistantAssetCategory> GetAssetCategories() const;
    TArray<FTopLevelAssetPath> GetAssetClasses() const;
    FString GetPluginConfigFilename() const;
    bool LoadPluginConfig(FString& OutError);
    bool SavePluginConfig(FString& OutError);
    bool LoadFromFile(const FString& Filename, FString& OutError);
    bool SaveToFile(const FString& Filename, FString& OutError);
    void MigrateLegacyConfig(const FString& Filename);
    bool Parse(TArray<FName>& OutRoots, TArray<ImportAssistant::FPathMapping>& OutMappings, FString& OutError) const;
    ImportAssistant::FMatchSettings MakeMatchSettings() const;
};


