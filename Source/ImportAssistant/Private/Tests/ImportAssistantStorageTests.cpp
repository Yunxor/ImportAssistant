#include "ImportAssistantStorageTemplates.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "ImportAssistantMatcher.h"
#include "ImportAssistantResultGroups.h"
#include "ImportAssistantSettings.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

using namespace ImportAssistant;

namespace
{
FImportAssistantStorageTemplate CharacterTemplate()
{
    FImportAssistantStorageTemplate Template;
    Template.TemplateName = TEXT("角色动画");
    Template.Description = TEXT("第一个词段代表角色名，动画放到角色的 Animations 目录");
    Template.NamePrefix = TEXT("AN_");
    Template.DirectoryTemplate = TEXT("/Game/Character/{Name}/Animations");
    FImportAssistantNameField& Name = Template.Fields.AddDefaulted_GetRef();
    Name.Position = 1;
    Name.Variable = TEXT("Name");
    Name.Description = TEXT("角色名");
    FImportAssistantNameField& Action = Template.Fields.AddDefaulted_GetRef();
    Action.Position = 2;
    Action.Variable = TEXT("Action");
    Action.Description = TEXT("动作名");
    return Template;
}

FRecordPtr AddRecord(FSearchSnapshot& Index, const FString& Package, const FString& Source)
{
    auto Record = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Record->PackageName = Package;
    Record->Name = FPackageName::GetShortName(Package);
    Record->ObjectPath = FSoftObjectPath(Package + TEXT(".") + Record->Name);
    if (!Source.IsEmpty())
    {
        Record->Sources.Add({Source, {NormalizePath(Source)}});
        Index.BySource.FindOrAdd(NormalizePath(Source)).Add(Record);
    }
    else { Index.UnknownRecords.Add(Record); }
    Index.Records.Add(Record);
    return Record;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStorageParsingTest, "ImportAssistant.StorageTemplates.Parsing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStorageParsingTest::RunTest(const FString&)
{
    FImportAssistantStorageTemplate Template = CharacterTemplate();
    TArray<FFileSearchScope> Scopes;
    TArray<FName> Roots;
    FString Error;
    const TArray<FString> Files = {TEXT("C:/AN_Male_Run_Loop.fbx"), TEXT("C:/AN_Female_Run.fbx"), TEXT("C:/an_Male_.fbx"),
        TEXT("C:/AN__Run.fbx"), TEXT("C:/Other_Male_Run.fbx")};
    TestTrue(TEXT("默认解析成功"), BuildStorageSearchPlan({Template}, true, EImportAssistantStorageSearchMode::PreferTemplate,
        EImportAssistantAssetCategory::Animation, Files, {FName(TEXT("/Game"))}, true, Scopes, Roots, Error));
    if (!TestEqual(TEXT("每个输入各有范围"), Scopes.Num(), Files.Num())) { return false; }
    TestTrue(TEXT("Name 替换保留大小写"), Scopes[0].TemplateRoots == TArray<FString>{TEXT("/Game/Character/Male/Animations")});
    TestTrue(TEXT("Female 不误解释为 Male"), Scopes[1].TemplateRoots == TArray<FString>{TEXT("/Game/Character/Female/Animations")});
    TestTrue(TEXT("未参与目录的空 Action 不阻止模板"), !Scopes[2].TemplateRoots.IsEmpty());
    TestTrue(TEXT("首段为空不能把第二段 Run 提前"), Scopes[3].TemplateRoots.IsEmpty());
    TestTrue(TEXT("空首段说明原因并回退"), Scopes[3].Details.Contains(TEXT("字段缺失或为空：Name")) && Scopes[3].SearchRoots.Contains(TEXT("/Game")));
    TestTrue(TEXT("前缀不匹配回退"), Scopes[4].TemplateRoots.IsEmpty() && Scopes[4].Details.Contains(TEXT("缺少前缀 AN_")));
    TestTrue(TEXT("预览解释字段位置与含义"), Scopes[0].Details.Contains(TEXT("Name = Male（第 1 段，角色名）")));
    TestTrue(TEXT("已按父目录合并索引"), Roots == TArray<FName>{FName(TEXT("/Game"))});
    Template.Delimiter = TEXT("::");
    Template.NamePrefix = TEXT("AN::");
    Template.Fields[0].Position = 2;
    Template.Fields[1].Position = 1;
    Template.DirectoryTemplate = TEXT("/Game/{Name}/{Name}");
    TestTrue(TEXT("多字符分隔符按字面解析"), BuildStorageSearchPlan({Template}, true, EImportAssistantStorageSearchMode::TemplateOnly,
        EImportAssistantAssetCategory::Animation, {TEXT("AN::Run::Male.fbx")}, {FName(TEXT("/Game"))}, false, Scopes, Roots, Error));
    TestTrue(TEXT("重复变量和非首段位置"), Scopes[0].TemplateRoots == TArray<FString>{TEXT("/Game/Male/Male")});
    Template = CharacterTemplate();
    BuildStorageSearchPlan({Template}, true, EImportAssistantStorageSearchMode::TemplateOnly, EImportAssistantAssetCategory::Animation,
        {TEXT("AN_.._Run.fbx")}, {FName(TEXT("/Game"))}, true, Scopes, Roots, Error);
    TestTrue(TEXT("非法变量值不能插入越级目录"), Scopes[0].TemplateRoots.IsEmpty() && Scopes[0].Details.Contains(TEXT("字段不能作为单个合法目录名")));
    Template = CharacterTemplate();
    Template.Fields[1].Variable = TEXT("name");
    Template.DirectoryTemplate = TEXT("/Game/{Name}/{name}");
    TestTrue(TEXT("大小写不同的两个变量可独立定义"), BuildStorageSearchPlan({Template}, true, EImportAssistantStorageSearchMode::TemplateOnly,
        EImportAssistantAssetCategory::Animation, {TEXT("AN_Male_Run.fbx")}, {FName(TEXT("/Game"))}, true, Scopes, Roots, Error));
    if (!Scopes.IsEmpty()) { TestTrue(TEXT("大小写不同的变量不会覆盖彼此"), Scopes[0].TemplateRoots == TArray<FString>{TEXT("/Game/Male/Run")}); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStorageValidationTest, "ImportAssistant.StorageTemplates.Validation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStorageValidationTest::RunTest(const FString&)
{
    auto Validate = [this](const FImportAssistantStorageTemplate& Template, const FString& Label)
    {
        TArray<FFileSearchScope> Scopes;
        TArray<FName> Roots;
        FString Error;
        const bool Result = BuildStorageSearchPlan({Template}, true, EImportAssistantStorageSearchMode::PreferTemplate,
            EImportAssistantAssetCategory::Animation, {TEXT("AN_Male_Run.fbx")}, {FName(TEXT("/Game"))}, true, Scopes, Roots, Error);
        TestFalse(Label, Result);
        TestFalse(TEXT("配置错误有解释"), Error.IsEmpty());
    };
    FImportAssistantStorageTemplate Template = CharacterTemplate();
    Template.DirectoryTemplate = TEXT("/Game/{Undefined}"); Validate(Template, TEXT("拒绝未定义变量"));
    Template.DirectoryTemplate = TEXT("/Game/{name}"); Validate(Template, TEXT("变量名区分大小写"));
    Template.DirectoryTemplate = TEXT("/Game/{Name"); Validate(Template, TEXT("拒绝不闭合变量"));
    Template.DirectoryTemplate = TEXT("/Game/Name}"); Validate(Template, TEXT("拒绝额外结束括号"));
    Template.DirectoryTemplate = TEXT("/{Name}/Animations"); Validate(Template, TEXT("根目录不能变量化"));
    Template.DirectoryTemplate = TEXT("/UnMountedImportAssistantRoot/{Name}"); Validate(Template, TEXT("拒绝未挂载根"));
    Template.DirectoryTemplate = TEXT("/Game/../Character/{Name}"); Validate(Template, TEXT("拒绝越级目录"));
    Template.DirectoryTemplate = TEXT("Game/Character/{Name}"); Validate(Template, TEXT("必须使用 UE 绝对包目录"));
    Template = CharacterTemplate(); Template.Fields[1].Variable = TEXT("Name"); Validate(Template, TEXT("拒绝重复变量"));
    Template = CharacterTemplate(); Template.Fields[0].Position = 0; Validate(Template, TEXT("段位必须从一开始"));
    Template = CharacterTemplate(); Template.Delimiter.Reset(); Validate(Template, TEXT("分隔符不能为空"));
    Template = CharacterTemplate(); Template.Fields[0].Variable = TEXT("Bad Name"); Validate(Template, TEXT("拒绝非法变量名"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStorageScopeTest, "ImportAssistant.StorageTemplates.SearchCompleteness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStorageScopeTest::RunTest(const FString&)
{
    const FImportAssistantStorageTemplate Template = CharacterTemplate();
    const TArray<FString> Files = {TEXT("C:/StorageTests/AN_Male_Run.fbx"), TEXT("C:/StorageTests/AN_Female_Run.fbx")};
    FSearchSnapshot Index;
    AddRecord(Index, TEXT("/Game/Character/Male/Animations/Run"), Files[0]);
    AddRecord(Index, TEXT("/Game/Legacy/MaleRun"), Files[0]);
    AddRecord(Index, TEXT("/Game/Character/Female/Animations/Run"), Files[1]);
    AddRecord(Index, TEXT("/Game/Character/Female/Animations/MaleCopy"), Files[0]);
    AddRecord(Index, TEXT("/Game/Outside/Copy"), Files[0]);
    AddRecord(Index, TEXT("/Game/Outside/Unknown"), TEXT(""));
    TArray<FFileSearchScope> Scopes;
    TArray<FName> Roots;
    FString Error;
    FMatchSettings Settings;
    std::atomic_bool Cancel(false);
    TestTrue(TEXT("生成优先补查计划"), BuildStorageSearchPlan({Template}, true, EImportAssistantStorageSearchMode::PreferTemplate,
        EImportAssistantAssetCategory::Animation, Files, {FName(TEXT("/Game/Legacy"))}, true, Scopes, Roots, Error));
    FSearchOutput Result = Match(Index, Files, Settings, {}, Cancel, Scopes);
    TestEqual(TEXT("已有模板命中仍发现原目录旧资产"), Result.Rows.Num(), 3);
    if (Result.Rows.Num() != 3) { return false; }
    const FRowPtr* Outside = Result.Rows.FindByPredicate([](const FRowPtr& Row) { return Row->Asset->PackageName == TEXT("/Game/Legacy/MaleRun"); });
    TestTrue(TEXT("补查资产独立标记且仍能勾选"), Outside && (*Outside)->bOutsideTemplate && (*Outside)->CanSelect());
    const FRowPtr* Leak = Result.Rows.FindByPredicate([](const FRowPtr& Row) { return Row->Asset->Name == TEXT("MaleCopy"); });
    TestTrue(TEXT("不串用另一个 FBX 的模板目录"), Leak == nullptr);
    TestEqual(TEXT("范围外未知资产不计入未知数"), Result.UnmatchedUnknownCount, 0);
    const auto Groups = BuildResultGroups(Files, Result.Rows, false, Scopes);
    TestEqual(TEXT("输入顺序保留"), Groups[0]->File, Files[0]);
    TestTrue(TEXT("组内优先展示模板目录资产"), !Groups[0]->Children[0]->Row->bOutsideTemplate);
    TestTrue(TEXT("组预览包含目录和字段解析"), Groups[0]->ScopeSummary.Contains(TEXT("/Game/Character/Male/Animations")) &&
        Groups[0]->ScopeDetails.Contains(TEXT("Name = Male")));
    TestEqual(TEXT("组标题 Tooltip 仍是文件路径"), Groups[0]->Tooltip, Files[0]);
    BuildStorageSearchPlan({Template}, true, EImportAssistantStorageSearchMode::TemplateOnly, EImportAssistantAssetCategory::Animation,
        Files, {FName(TEXT("/Game/Legacy"))}, true, Scopes, Roots, Error);
    Result = Match(Index, Files, Settings, {}, Cancel, Scopes);
    TestEqual(TEXT("仅模板只保留两个对应目录资产"), Result.Rows.Num(), 2);
    TestTrue(TEXT("仅模板限制必须可见"), Scopes[0].bLimitedToTemplates && Scopes[0].Summary.Contains(TEXT("未检查模板目录外资产")));
    AddRecord(Index, TEXT("/Game/Character/Male/Animations/Sub/Run"), Files[0]);
    BuildStorageSearchPlan({Template}, true, EImportAssistantStorageSearchMode::TemplateOnly, EImportAssistantAssetCategory::Animation,
        {Files[0]}, {FName(TEXT("/Game"))}, false, Scopes, Roots, Error);
    Result = Match(Index, {Files[0]}, Settings, {}, Cancel, Scopes);
    TestEqual(TEXT("非递归不包含子目录"), Result.Rows.Num(), 1);
    Settings.bMatchPath = false;
    Settings.bMatchKeyword = true;
    Settings.bAllKeywords = false;
    BuildStorageSearchPlan({Template}, true, EImportAssistantStorageSearchMode::TemplateOnly, EImportAssistantAssetCategory::Animation,
        Files, {FName(TEXT("/Game/Legacy"))}, false, Scopes, Roots, Error);
    Result = Match(Index, Files, Settings, {}, Cancel, Scopes);
    TestTrue(TEXT("关键词查询也不能越过每文件目录范围"), Result.Rows.ContainsByPredicate([](const FRowPtr& Row)
        { return Row->File.EndsWith(TEXT("AN_Male_Run.fbx")) && Row->Asset->PackageName == TEXT("/Game/Character/Male/Animations/Run"); }));
    TestFalse(TEXT("Male 的 Run 关键词不能命中 Female/Run"), Result.Rows.ContainsByPredicate([](const FRowPtr& Row)
        { return Row->File.EndsWith(TEXT("AN_Male_Run.fbx")) && Row->Asset->PackageName.Contains(TEXT("/Female/")); }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStorageUnionTest, "ImportAssistant.StorageTemplates.CategoriesAndUnion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStorageUnionTest::RunTest(const FString&)
{
    TArray<FImportAssistantStorageTemplate> Templates = {CharacterTemplate()};
    Templates.Add(CharacterTemplate());
    Templates[1].TemplateName = TEXT("更深层动画");
    Templates[1].DirectoryTemplate += TEXT("/Locomotion");
    Templates.Add(CharacterTemplate());
    Templates[2].AssetCategory = EImportAssistantAssetCategory::SkeletalMesh;
    Templates[2].TemplateName = TEXT("骨骼模型");
    Templates[2].DirectoryTemplate = TEXT("/Game/Character/{Name}/Meshes");
    Templates[2].NamePrefix = TEXT("SK_");
    TArray<FFileSearchScope> Scopes;
    TArray<FName> Roots;
    FString Error;
    BuildStorageSearchPlan(Templates, true, EImportAssistantStorageSearchMode::TemplateOnly, EImportAssistantAssetCategory::Animation,
        {TEXT("AN_Male_Run.fbx"), TEXT("SK_Male.fbx")}, {FName(TEXT("/Game/Legacy"))}, true, Scopes, Roots, Error);
    TestEqual(TEXT("递归模式去除被父目录覆盖的重复子目录"), Scopes[0].TemplateRoots.Num(), 1);
    TestTrue(TEXT("合并范围仍保留全部命中模板依据"), Scopes[0].Details.Contains(TEXT("更深层动画")));
    TestTrue(TEXT("动画窗口不应用骨骼模型模板"), Scopes[1].TemplateRoots.IsEmpty() && Scopes[1].SearchRoots == TArray<FString>{TEXT("/Game/Legacy")});
    BuildStorageSearchPlan(Templates, true, EImportAssistantStorageSearchMode::TemplateOnly, EImportAssistantAssetCategory::SkeletalMesh,
        {TEXT("SK_Male.fbx")}, {FName(TEXT("/Game"))}, true, Scopes, Roots, Error);
    TestTrue(TEXT("同一解析器可用于模型类别"), Scopes[0].TemplateRoots == TArray<FString>{TEXT("/Game/Character/Male/Meshes")});
    BuildStorageSearchPlan(Templates, true, EImportAssistantStorageSearchMode::TemplateOnly, EImportAssistantAssetCategory::Animation,
        {TEXT("AN_Male_Run.fbx")}, {FName(TEXT("/Game"))}, false, Scopes, Roots, Error);
    TestEqual(TEXT("不递归时必须保留父目录与子目录"), Scopes[0].TemplateRoots.Num(), 2);
    Templates[0].DirectoryTemplate = TEXT("/Game/{Bad}");
    TestTrue(TEXT("关闭模板保持旧行为并忽略未启用功能的配置"), BuildStorageSearchPlan(Templates, false,
        EImportAssistantStorageSearchMode::PreferTemplate, EImportAssistantAssetCategory::Animation,
        {TEXT("AN_Male.fbx")}, {FName(TEXT("/Game"))}, true, Scopes, Roots, Error));
    TestTrue(TEXT("关闭不生成逐文件限制"), Scopes.IsEmpty() && Roots == TArray<FName>{FName(TEXT("/Game"))});
    TestTrue(TEXT("目录边界不按子串误判"), !IsDirectoryInRoots(TEXT("/Game/Character/MaleExtra"), {TEXT("/Game/Character/Male")}, true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStorageConfigTest, "ImportAssistant.StorageTemplates.ConfigRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStorageConfigTest::RunTest(const FString&)
{
    const FString Filename = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ImportAssistantTests") /
        FGuid::NewGuid().ToString(EGuidFormats::Digits) / TEXT("StorageTemplates.ini"));
    TStrongObjectPtr<UImportAssistantSettings> Settings(NewObject<UImportAssistantSettings>());
    Settings->bUseStorageTemplates = true;
    Settings->StorageSearchMode = EImportAssistantStorageSearchMode::TemplateOnly;
    Settings->StorageTemplates = {CharacterTemplate()};
    Settings->StorageTemplates.Add(CharacterTemplate());
    Settings->StorageTemplates[1].AssetCategory = EImportAssistantAssetCategory::StaticMesh;
    Settings->StorageTemplates[1].TemplateName = TEXT("场景模型");
    FString Error;
    if (!TestTrue(TEXT("嵌套模板 Config 保存"), Settings->SaveToFile(Filename, Error))) { AddError(Error); return false; }
    TStrongObjectPtr<UImportAssistantSettings> Loaded(NewObject<UImportAssistantSettings>());
    if (!TestTrue(TEXT("模板 Config 读取"), Loaded->LoadFromFile(Filename, Error))) { AddError(Error); return false; }
    if (!TestEqual(TEXT("模板数量保持"), Loaded->StorageTemplates.Num(), 2)) { return false; }
    TestTrue(TEXT("开关与枚举保存"), Loaded->bUseStorageTemplates && Loaded->StorageSearchMode == EImportAssistantStorageSearchMode::TemplateOnly);
    TestEqual(TEXT("模型类别未丢失"), Loaded->StorageTemplates[1].AssetCategory, EImportAssistantAssetCategory::StaticMesh);
    TestEqual(TEXT("嵌套字段数量"), Loaded->StorageTemplates[0].Fields.Num(), 2);
    if (Loaded->StorageTemplates[0].Fields.Num() == 2)
    {
        TestEqual(TEXT("字段序号"), Loaded->StorageTemplates[0].Fields[1].Position, 2);
        TestEqual(TEXT("字段中文说明"), Loaded->StorageTemplates[0].Fields[1].Description, FString(TEXT("动作名")));
    }
    TestEqual(TEXT("目录模板保留变量"), Loaded->StorageTemplates[0].DirectoryTemplate, FString(TEXT("/Game/Character/{Name}/Animations")));
    Loaded->StorageTemplates.Reset();
    Loaded->SaveToFile(Filename, Error);
    Settings->LoadFromFile(Filename, Error);
    TestTrue(TEXT("清空模板后重新读取不恢复旧项"), Settings->StorageTemplates.IsEmpty());
    AddInfo(TEXT("模板配置样本：") + Filename);
    return true;
}
#endif



