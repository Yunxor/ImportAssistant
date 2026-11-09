#include "ImportAssistantMatcher.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ImportAssistantAssetIndex.h"
#include "SImportAssistantWindow.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Framework/Application/SlateApplication.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"

using namespace ImportAssistant;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantPathTest, "ImportAssistant.Matching.Paths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantPathTest::RunTest(const FString&)
{
    TestEqual(TEXT("统一大小写和相对段"), NormalizePath(TEXT("C:\\Art\\Hero\\..\\Run.FBX")), FString(TEXT("c:/art/run.fbx")));
    const TArray<FPathMapping> Rules = { { TEXT("c:/art"), TEXT("d:/source") }, { TEXT("c:/art/hero"), TEXT("e:/hero") } };
    TestEqual(TEXT("最长目录前缀优先"), MapPath(TEXT("c:/art/hero/run.fbx"), Rules)[0], FString(TEXT("e:/hero/run.fbx")));
    TestTrue(TEXT("不能匹配目录名子串"), MapPath(TEXT("c:/artist/run.fbx"), Rules).IsEmpty());
    TestEqual(TEXT("相同优先级映射保留歧义"), MapPath(TEXT("c:/art/run.fbx"),
        {{ TEXT("c:/art"), TEXT("d:/one") }, { TEXT("c:/art"), TEXT("e:/two") }}).Num(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantKeywordTest, "ImportAssistant.Matching.Keywords", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantKeywordTest::RunTest(const FString&)
{
    FMatchSettings Settings;
    Settings.IgnorePrefixes = { TEXT("AN_") };
    Settings.IgnoreSuffixes = { TEXT("_Export") };
    TestTrue(TEXT("去除前后缀并忽略大小写"), MatchesKeywords(TEXT("hero_run_loop"), TEXT("C:/AN_Hero_Run_Export.fbx"), Settings));
    TestFalse(TEXT("全部关键词不允许缺词"), MatchesKeywords(TEXT("hero_walk"), TEXT("Hero_Run.fbx"), Settings));
    Settings.bAllKeywords = false;
    TestTrue(TEXT("任意关键词"), MatchesKeywords(TEXT("hero_walk"), TEXT("Hero_Run.fbx"), Settings));
    TestFalse(TEXT("空关键词不会匹配全部"), MatchesKeywords(TEXT("hero"), TEXT("AN__Export.fbx"), Settings));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantMatchingTest, "ImportAssistant.Matching.OneToManyAndConflicts", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantMatchingTest::RunTest(const FString&)
{
    FSearchSnapshot Index;
    for (int32 Number = 0; Number < 2; ++Number)
    {
        auto Record = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
        Record->Name = FString::Printf(TEXT("Hero_Run_%d"), Number);
        Record->ObjectPath = FSoftObjectPath(FString::Printf(TEXT("/Game/Test/%s.%s"), *Record->Name, *Record->Name));
        Record->Sources.Add({TEXT("C:/ImportAssistantMissing/Run.fbx"), {TEXT("c:/importassistantmissing/run.fbx")}});
        Index.Records.Add(Record);
        Index.BySource.FindOrAdd(TEXT("c:/importassistantmissing/run.fbx")).Add(Record);
    }
    auto Unknown = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Unknown->Name = TEXT("Unknown");
    Unknown->ObjectPath = FSoftObjectPath(TEXT("/Game/Test/Unknown.Unknown"));
    Index.Records.Add(Unknown);
    Index.UnknownRecords.Add(Unknown);
    std::atomic_bool Cancel(false);
    FMatchSettings Settings;
    FSearchOutput Result = Match(Index, {TEXT("C:/ImportAssistantMissing/Run.fbx")}, Settings, {}, Cancel);
    TestEqual(TEXT("两个同源动画和一个未知来源"), Result.Rows.Num(), 3);
    TestEqual(TEXT("未知来源计数"), Result.UnknownCount, 1);
    Settings.bMatchKeyword = true;
    Result = Match(Index, {TEXT("C:/ImportAssistantMissing/Run.fbx"), TEXT("D:/Other/Run.fbx")}, Settings, {}, Cancel);
    TestEqual(TEXT("路径和关键词取并集且去重"), Result.Rows.Num(), 5);
    int32 Conflicts = 0;
    for (const FRowPtr& Row : Result.Rows) { Conflicts += Row->bFileConflict ? 1 : 0; TestFalse(TEXT("不自动勾选"), Row->bSelected); }
    TestEqual(TEXT("两个资产的两种输入均标记冲突"), Conflicts, 4);
    Cancel.store(true);
    TestTrue(TEXT("取消任务不发布部分结果"), Match(Index, {TEXT("C:/Run.fbx")}, Settings, {}, Cancel).Rows.IsEmpty());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantMetadataTest, "ImportAssistant.Matching.Metadata", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantMetadataTest::RunTest(const FString&)
{
    FAssetImportInfo Info;
    Info.Insert(FAssetImportInfo::FSourceFile(TEXT("C:/Source/A.fbx")));
    Info.Insert(FAssetImportInfo::FSourceFile(TEXT("D:/Source/B.fbx")));
    FAssetDataTagMap Tags;
    Tags.Add(UObject::SourceFileTagName(), Info.ToJson());
    const FAssetData Data(FName(TEXT("/Game/Test/Walk")), FName(TEXT("/Game/Test")), FName(TEXT("Walk")),
        UAnimSequence::StaticClass()->GetClassPathName(), MoveTemp(Tags));
    FRecordPtr Record = MakeRecord(Data, TEXT(""));
    TestEqual(TEXT("保存全部来源槽位"), Record->Sources.Num(), 2);
    TestEqual(TEXT("检索记录保留真实资产类型而不加载对象"), Record->AssetClassPath, UAnimSequence::StaticClass()->GetClassPathName());
    FSearchSnapshot Index;
    Index.Records.Add(Record);
    for (const FSourceRecord& Source : Record->Sources)
    {
        for (const FString& Key : Source.Candidates) { Index.BySource.FindOrAdd(Key).Add(Record); }
    }
    std::atomic_bool Cancel(false);
    FMatchSettings Settings;
    const FSearchOutput Result = Match(Index, {TEXT("D:/Source/B.fbx")}, Settings, {}, Cancel);
    TestEqual(TEXT("非主来源可检索"), Result.Rows.Num(), 1);
    if (!Result.Rows.IsEmpty()) { TestEqual(TEXT("来源槽位不丢失"), Result.Rows[0]->SourceIndex, 1); }
    FAssetDataTagMap LegacyTags;
    LegacyTags.Add(FName(TEXT("SourceFile")), TEXT("C:/Old/Walk.fbx"));
    const FAssetData LegacyData(FName(TEXT("/Game/Test/Old")), FName(TEXT("/Game/Test")), FName(TEXT("Old")),
        UAnimSequence::StaticClass()->GetClassPathName(), MoveTemp(LegacyTags));
    TestEqual(TEXT("兼容旧来源 Tag"), MakeRecord(LegacyData, TEXT(""))->Sources.Num(), 1);
    FAssetDataTagMap BrokenTags;
    BrokenTags.Add(UObject::SourceFileTagName(), TEXT("invalid"));
    const FAssetData BrokenData(FName(TEXT("/Game/Test/Broken")), FName(TEXT("/Game/Test")), FName(TEXT("Broken")),
        UAnimSequence::StaticClass()->GetClassPathName(), MoveTemp(BrokenTags));
    TestTrue(TEXT("错误元数据保留为未知来源"), MakeRecord(BrokenData, TEXT(""))->Sources.IsEmpty());
    TestEqual(TEXT("未命中输入也被记录"), Match(Index, {TEXT("C:/Missing/Z.fbx")}, Settings, {}, Cancel).UnmatchedFiles.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantSettingsTest, "ImportAssistant.Settings.Validation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantSettingsTest::RunTest(const FString&)
{
    TStrongObjectPtr<UImportAssistantSettings> Settings(NewObject<UImportAssistantSettings>());
    TArray<FName> Roots;
    TArray<FPathMapping> Mappings;
    FString Error;
    TestTrue(TEXT("默认工程范围有效"), Settings->Parse(Roots, Mappings, Error));
    Settings->MappingsText = TEXT("C:/Old => D:/New");
    TestTrue(TEXT("接受绝对目录映射"), Settings->Parse(Roots, Mappings, Error));
    Settings->MappingsText = TEXT("Relative => D:/New");
    TestFalse(TEXT("拒绝相对目录映射"), Settings->Parse(Roots, Mappings, Error));
    Settings->AssetDirectories[0].Path = TEXT("/UnMountedImportAssistantRoot");
    TestFalse(TEXT("拒绝未挂载资产根"), Settings->Parse(Roots, Mappings, Error));
    Settings->AssetDirectories.Reset();
    TestFalse(TEXT("空目录数组不能退化成全工程搜索"), Settings->Parse(Roots, Mappings, Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantWidgetTest, "ImportAssistant.Editor.WidgetConstruction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantWidgetTest::RunTest(const FString&)
{
    if (!FSlateApplication::IsInitialized()) { AddError(TEXT("Slate 未初始化")); return false; }
    TSharedRef<SImportAssistantWindow> Window = SNew(SImportAssistantWindow).LoadSavedSettings(false);
    Window->SlatePrepass();
    TestTrue(TEXT("空闲窗口可以关闭"), Window->CanClose());
    TestTrue(TEXT("窗口有有效布局尺寸"), Window->GetDesiredSize().Y > 0);
    return true;
}

class FImportAssistantIndexCommand : public IAutomationLatentCommand
{
public:
    explicit FImportAssistantIndexCommand(FAutomationTestBase* InTest) : Test(InTest), Start(FPlatformTime::Seconds())
    {
        Index.Configure({FName(TEXT("/Game"))}, true);
    }

    virtual bool Update() override
    {
        Index.Tick();
        if (!Index.IsReady())
        {
            if (FPlatformTime::Seconds() - Start > 120) { Test->AddError(TEXT("只读索引在 120 秒内未就绪")); return true; }
            return false;
        }
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
        TArray<FAssetData> Expected;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Expected);
        const auto Snapshot = Index.Snapshot();
        TSet<FSoftObjectPath> Indexed;
        for (const FRecordPtr& Record : Snapshot->Records) { Indexed.Add(Record->ObjectPath); }
        Test->TestEqual(TEXT("只读索引数量与主线程注册表基准一致"), Indexed.Num(), Expected.Num());
        for (const FAssetData& Data : Expected) { Test->TestTrue(TEXT("基准动画没有漏检：") + Data.AssetName.ToString(), Indexed.Contains(Data.GetSoftObjectPath())); }
        Test->AddInfo(FString::Printf(TEXT("只读索引 %d 个动画，等待与构建合计 %.3f 秒（含资产发现，不是热查询性能）"),
            Indexed.Num(), FPlatformTime::Seconds() - Start));
        return true;
    }
private:
    FAutomationTestBase* Test;
    double Start;
    FAssetIndex Index;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantIndexTest, "ImportAssistant.Editor.ReadOnlyIndex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantIndexTest::RunTest(const FString&)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FImportAssistantIndexCommand>(this));
    return true;
}
class FImportAssistantMemoryCommand : public IAutomationLatentCommand
{
public:
    explicit FImportAssistantMemoryCommand(FAutomationTestBase* InTest) : Test(InTest), Start(FPlatformTime::Seconds())
    {
        Root = TEXT("/Game/__ImportAssistantMemory_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        UPackage* Package = CreatePackage(*(Root / TEXT("Probe")));
        Animation.Reset(NewObject<UAnimSequence>(Package, TEXT("Probe"), RF_Public | RF_Standalone));
        Animation->AssetImportData = NewObject<UAssetImportData>(Animation.Get());
        Animation->AssetImportData->SourceData.SourceFiles.Emplace(TEXT("C:/Memory/Before.fbx"));
        FAssetRegistryModule::AssetCreated(Animation.Get());
        Index.Configure({FName(Root)}, true);
    }
    ~FImportAssistantMemoryCommand()
    {
        FAssetRegistryModule::AssetDeleted(Animation.Get());
        Animation->ClearFlags(RF_Public | RF_Standalone);
        Animation->SetFlags(RF_Transient);
        Animation->GetPackage()->SetFlags(RF_Transient);
        Animation->GetPackage()->SetDirtyFlag(false);
    }
    virtual bool Update() override
    {
        Index.Tick();
        if (!Index.IsReady())
        {
            if (FPlatformTime::Seconds() - Start > 30) { Test->AddError(TEXT("内存索引测试超时")); return true; }
            return false;
        }
        const auto Snapshot = Index.Snapshot();
        Test->TestEqual(TEXT("未保存动画被纳入索引"), Snapshot->Records.Num(), 1);
        if (Snapshot->Records.IsEmpty()) { return true; }
        if (Phase == 0)
        {
            Test->TestEqual(TEXT("读取未保存来源"), Snapshot->Records[0]->Sources[0].Original, FString(TEXT("C:/Memory/Before.fbx")));
            Animation->AssetImportData->SourceData.SourceFiles[0].RelativeFilename = TEXT("D:/Memory/After.fbx");
            Animation->MarkPackageDirty();
            Phase = 1;
            return false;
        }
        Test->TestEqual(TEXT("无来源专用事件时由脏包事件更新索引"), Snapshot->Records[0]->Sources[0].Original, FString(TEXT("D:/Memory/After.fbx")));
        Test->TestTrue(TEXT("旧来源倒排项被移除"), !Snapshot->BySource.Contains(TEXT("c:/memory/before.fbx")));
        Test->TestTrue(TEXT("新来源倒排项已创建"), Snapshot->BySource.Contains(TEXT("d:/memory/after.fbx")));
        return true;
    }
private:
    FAutomationTestBase* Test;
    double Start;
    FString Root;
    int32 Phase = 0;
    TStrongObjectPtr<UAnimSequence> Animation;
    FAssetIndex Index;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantMemoryTest, "ImportAssistant.Editor.UnsavedSourceUpdates", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantMemoryTest::RunTest(const FString&)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FImportAssistantMemoryCommand>(this));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantConfigTest, "ImportAssistant.Settings.ConfigRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantConfigTest::RunTest(const FString&)
{
    TStrongObjectPtr<UImportAssistantSettings> Settings(NewObject<UImportAssistantSettings>());
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("ImportAssistantTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Filename = FPaths::ConvertRelativePathToFull(Directory / TEXT("Settings.ini"));
    Settings->AssetDirectories[0].Path = TEXT("/Game/Characters");
    Settings->AssetDirectories.AddDefaulted_GetRef().Path = TEXT("/Game/Animations");
    Settings->IgnorePrefixes = { TEXT("AN_"), TEXT("前缀;保留"), TEXT("AN_") };
    Settings->IgnoreSuffixes = { TEXT("_Export"), TEXT("_Preview") };
    Settings->MappingsText = TEXT("C:/Old => D:/New\nE:/旧目录 => F:/源文件");
    Settings->bMatchKeyword = true;
    Settings->bAllKeywords = false;
    FString Error;
    if (!TestTrue(TEXT("UE Config 保存数组配置"), Settings->SaveToFile(Filename, Error))) { AddError(Error); return false; }
    TStrongObjectPtr<UImportAssistantSettings> Loaded(NewObject<UImportAssistantSettings>());
    if (!TestTrue(TEXT("从实际磁盘重新载入配置"), Loaded->LoadFromFile(Filename, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("目录数组数量"), Loaded->AssetDirectories.Num(), 2);
    if (Loaded->AssetDirectories.Num() == 2)
    {
        TestEqual(TEXT("目录顺序一"), Loaded->AssetDirectories[0].Path, FString(TEXT("/Game/Characters")));
        TestEqual(TEXT("目录顺序二"), Loaded->AssetDirectories[1].Path, FString(TEXT("/Game/Animations")));
    }
    TestTrue(TEXT("前缀数组保留顺序、重复项、分号及中文"), Loaded->IgnorePrefixes == Settings->IgnorePrefixes);
    TestTrue(TEXT("后缀数组保留顺序"), Loaded->IgnoreSuffixes == Settings->IgnoreSuffixes);
    TestEqual(TEXT("多行映射保留"), Loaded->MappingsText, Settings->MappingsText);
    TestTrue(TEXT("布尔设置保留"), Loaded->bMatchKeyword && !Loaded->bAllKeywords);
    const FMatchSettings Snapshot = Loaded->MakeMatchSettings();
    Loaded->IgnorePrefixes.Reset();
    Loaded->IgnoreSuffixes.Reset();
    Loaded->AssetDirectories.Reset();
    Loaded->bMatchKeyword = false;
    TestTrue(TEXT("匹配快照与 UObject 独立"), Snapshot.bMatchKeyword && Snapshot.IgnorePrefixes == Settings->IgnorePrefixes);
    TestTrue(TEXT("可以保存清空的数组"), Loaded->SaveToFile(Filename, Error));
    TestTrue(TEXT("可以重新读取空数组"), Settings->LoadFromFile(Filename, Error));
    TestTrue(TEXT("空目录不会回退成 /Game"), Settings->AssetDirectories.IsEmpty());
    TestTrue(TEXT("已清空的前后缀不会残留"), Settings->IgnorePrefixes.IsEmpty() && Settings->IgnoreSuffixes.IsEmpty());
    TestEqual(TEXT("使用要求的 Config 分类"), Settings->GetClass()->ClassConfigName, FName(TEXT("EditorPerProjectUserSettings")));
    TestTrue(TEXT("插件配置路径位于当前插件"), Settings->GetPluginConfigFilename().StartsWith(
        FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin(TEXT("ImportAssistant"))->GetBaseDir()) + TEXT("/")));
    AddInfo(TEXT("配置往返测试文件保留于：") + Filename);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantMigrationTest, "ImportAssistant.Settings.LegacyMigration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantMigrationTest::RunTest(const FString&)
{
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("ImportAssistantTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString Filename = FPaths::ConvertRelativePathToFull(Directory / TEXT("Legacy.ini"));
    const FString Legacy = TEXT("[ImportAssistant]\nRoots=/Game/Characters; /Game/Animations\nIgnorePrefixes=AN_; SK_\n"
        "IgnoreSuffixes=_Export;_Preview\nRecursive=False\nMatchPath=False\nMatchKeyword=True\nAllKeywords=False\nCheckout=True\n");
    if (!TestTrue(TEXT("写入独立旧格式样本"), FFileHelper::SaveStringToFile(Legacy, *Filename))) { return false; }
    TStrongObjectPtr<UImportAssistantSettings> Settings(NewObject<UImportAssistantSettings>());
    Settings->MigrateLegacyConfig(Filename);
    TestEqual(TEXT("迁移目录数组"), Settings->AssetDirectories.Num(), 2);
    TestTrue(TEXT("迁移前缀并去除边缘空格"), Settings->IgnorePrefixes == TArray<FString>{TEXT("AN_"), TEXT("SK_")});
    TestTrue(TEXT("迁移后缀"), Settings->IgnoreSuffixes == TArray<FString>{TEXT("_Export"), TEXT("_Preview")});
    TestTrue(TEXT("迁移旧开关"), !Settings->bRecursive && !Settings->bMatchPath && Settings->bMatchKeyword && !Settings->bAllKeywords);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantGroupingTest, "ImportAssistant.Editor.FbxGroups", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantGroupingTest::RunTest(const FString&)
{
    const TArray<FString> Files = { TEXT("D:/Second/Run.fbx"), TEXT("C:/First/Run.fbx"), TEXT("E:/Empty/Stop.fbx") };
    TArray<FRowPtr> Rows;
    for (int32 Number = 0; Number < 3; ++Number)
    {
        auto Asset = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
        Asset->ObjectPath = FSoftObjectPath(FString::Printf(TEXT("/Game/Group/Animation%d.Animation%d"), Number, Number));
        FRowPtr Row = MakeShared<FMatchRow>();
        Row->Asset = Asset;
        Row->File = Number < 2 ? Files[1 - Number] : FString();
        Rows.Add(Row);
    }
    const auto Groups = BuildResultGroups(Files, Rows, false);
    TestEqual(TEXT("三个 FBX 加一个未知来源组"), Groups.Num(), 4);
    if (Groups.Num() != 4) { return false; }
    TestEqual(TEXT("遵循添加顺序而非路径字典顺序"), Groups[0]->File, Files[0]);
    TestEqual(TEXT("同名不同目录分为独立组"), Groups[1]->File, Files[1]);
    TestEqual(TEXT("组标题不含扩展名"), Groups[0]->Title, FString(TEXT("Run")));
    TestEqual(TEXT("Tooltip 为完整文件路径"), Groups[0]->Tooltip, Files[0]);
    TestTrue(TEXT("分组引用原匹配行，勾选和状态不被复制"), Groups[0]->Children[0]->Row == Rows[1]);
    Groups[0]->Children[0]->Row->bSelected = true;
    TestTrue(TEXT("组内勾选直接作用于原执行行"), Rows[1]->bSelected);
    TestFalse(TEXT("零匹配组使用不可勾选占位行"), Groups[2]->Children[0]->Row.IsValid());
    TestEqual(TEXT("零匹配组仍保留原文件标题"), Groups[2]->Title, FString(TEXT("Stop")));
    TestTrue(TEXT("零匹配占位行封闭小组底边"), Groups[2]->Children[0]->bLastInGroup);
    for (const FResultNodePtr& Group : Groups) { TestTrue(TEXT("每组末尾标识用于完整边框"), Group->Children.Last()->bLastInGroup); }
    TestTrue(TEXT("未知来源不强行归入 FBX"), Groups[3]->File.IsEmpty() && Groups[3]->Children[0]->Row == Rows[2]);
    const auto Pending = BuildResultGroups(Files, {}, true);
    TestEqual(TEXT("未完成检索不能误报零匹配"), Pending[0]->Children[0]->Title, FString(TEXT("等待检索完成…")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantKeywordDetailsTest, "ImportAssistant.Matching.KeywordDetails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantKeywordDetailsTest::RunTest(const FString&)
{
    FMatchSettings Settings;
    Settings.bMatchKeyword = true;
    Settings.bMatchPath = false;
    Settings.IgnorePrefixes = { TEXT("AN_") };
    Settings.IgnoreSuffixes = { TEXT("_Export") };
    TArray<FString> Hits;
    TestTrue(TEXT("全部模式保持原匹配规则"), MatchesKeywords(TEXT("hero_run_loop"), TEXT("C:/AN_Hero_Run_Export.fbx"), Settings, &Hits));
    TestTrue(TEXT("详情去掉忽略的前后缀"), Hits == TArray<FString>{ TEXT("Hero"), TEXT("Run") });
    TestFalse(TEXT("全部模式缺词仍然不命中"), MatchesKeywords(TEXT("hero_walk"), TEXT("Hero_Run.fbx"), Settings, &Hits));
    TestTrue(TEXT("不命中时不泄露部分关键词"), Hits.IsEmpty());
    Settings.bAllKeywords = false;
    TestTrue(TEXT("任意模式能命中"), MatchesKeywords(TEXT("hero_run_loop"), TEXT("Hero_Fly_Run.fbx"), Settings, &Hits));
    TestTrue(TEXT("任意模式列出所有实际命中的词，不含 Fly"), Hits == TArray<FString>{ TEXT("Hero"), TEXT("Run") });
    TestFalse(TEXT("空关键词不会命中"), MatchesKeywords(TEXT("hero_run_loop"), TEXT("AN__Export.fbx"), Settings, &Hits));
    TestTrue(TEXT("空关键词清除旧输出"), Hits.IsEmpty());

    auto Record = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Record->Name = TEXT("hero_run_loop");
    Record->ObjectPath = FSoftObjectPath(TEXT("/Game/Details/Run.Run"));
    FSearchSnapshot Index;
    Index.Records.Add(Record);
    std::atomic_bool Cancel(false);
    const FSearchOutput Any = Match(Index, {TEXT("C:/AN_Hero_Fly_Run_Export.fbx")}, Settings, {}, Cancel);
    if (!TestEqual(TEXT("任意关键词结果数量"), Any.Rows.Num(), 1)) { return false; }
    TestEqual(TEXT("Tooltip 仅显示实际命中内容"), Any.Rows[0]->ReasonTooltip,
        FString(TEXT("名称关键词匹配：任意关键词\n命中关键词：Hero、Run\n匹配对象：hero_run_loop")));
    Settings.bAllKeywords = true;
    const FSearchOutput All = Match(Index, {TEXT("C:/AN_Hero_Run_Export.fbx")}, Settings, {}, Cancel);
    if (!TestEqual(TEXT("全部关键词结果数量"), All.Rows.Num(), 1)) { return false; }
    TestTrue(TEXT("全部模式详情有正确模式与词"), All.Rows[0]->ReasonTooltip.Contains(TEXT("全部关键词\n命中关键词：Hero、Run")));
    Settings.IgnorePrefixes.Reset();
    TestTrue(TEXT("检索后修改设置不改变原 Tooltip"), Any.Rows[0]->ReasonTooltip.Contains(TEXT("任意关键词\n命中关键词：Hero、Run")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantPathDetailsTest, "ImportAssistant.Matching.PathDetails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantPathDetailsTest::RunTest(const FString&)
{
    auto Record = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Record->Name = TEXT("Hero_Run");
    Record->ObjectPath = FSoftObjectPath(TEXT("/Game/Details/Run.Run"));
    Record->Sources.Add({TEXT("C:/ImportAssistantDetails/Hero/Run.fbx"), {TEXT("c:/importassistantdetails/hero/run.fbx")}});
    FSearchSnapshot Index;
    Index.Records.Add(Record);
    Index.BySource.Add(Record->Sources[0].Candidates[0], {Record});
    FMatchSettings Settings;
    Settings.bMatchKeyword = true;
    std::atomic_bool Cancel(false);
    const FSearchOutput Direct = Match(Index, {TEXT("C:/ImportAssistantDetails/Hero/Run.fbx")}, Settings, {}, Cancel);
    if (!TestEqual(TEXT("精确路径和关键词并集仍只生成一行"), Direct.Rows.Num(), 1)) { return false; }
    const FString& DirectTip = Direct.Rows[0]->ReasonTooltip;
    TestTrue(TEXT("路径详情包含原始来源"), DirectTip.Contains(TEXT("资产记录：C:/ImportAssistantDetails/Hero/Run.fbx")));
    TestTrue(TEXT("路径详情包含解析路径"), DirectTip.Contains(TEXT("解析路径：c:/importassistantdetails/hero/run.fbx")));
    TestTrue(TEXT("路径详情包含拖入文件"), DirectTip.Contains(TEXT("本次 FBX：C:/ImportAssistantDetails/Hero/Run.fbx")));
    TestTrue(TEXT("组合命中分段展示"), DirectTip.Contains(TEXT("\n\n名称关键词匹配：全部关键词\n命中关键词：Run")));

    Settings.bMatchKeyword = false;
    const TArray<FPathMapping> Rules = {{TEXT("c:/importassistantdetails"), TEXT("e:/broad")},
        {TEXT("c:/importassistantdetails/hero"), TEXT("d:/selected")}};
    const FSearchOutput Mapped = Match(Index, {TEXT("D:/Selected/Run.fbx")}, Settings, Rules, Cancel);
    if (!TestEqual(TEXT("映射后找到同源行"), Mapped.Rows.Num(), 1)) { return false; }
    const FString& MapTip = Mapped.Rows[0]->ReasonTooltip;
    TestTrue(TEXT("仅展示实际采用的最长目录映射"), MapTip.Contains(TEXT("映射规则：c:/importassistantdetails/hero => d:/selected")));
    TestFalse(TEXT("不显示被覆盖的较短规则"), MapTip.Contains(TEXT("e:/broad")));
    TestTrue(TEXT("包含映射前后路径"), MapTip.Contains(TEXT("映射前：c:/importassistantdetails/hero/run.fbx\n映射后：d:/selected/run.fbx")));
    TestTrue(TEXT("保留 FBX 显示大小写"), MapTip.Contains(TEXT("本次 FBX：D:/Selected/Run.fbx")));

    TArray<FPathMapping> AmbiguousRules = Rules;
    AmbiguousRules.Add({TEXT("c:/importassistantdetails/hero"), TEXT("e:/alternative")});
    const FSearchOutput Ambiguous = Match(Index, {TEXT("D:/Selected/Run.fbx")}, Settings, AmbiguousRules, Cancel);
    if (!TestEqual(TEXT("歧义映射仍可展示候选"), Ambiguous.Rows.Num(), 1)) { return false; }
    TestTrue(TEXT("详情提示映射歧义且不能选择"), Ambiguous.Rows[0]->bSourceAmbiguous && !Ambiguous.Rows[0]->CanSelect() &&
        Ambiguous.Rows[0]->ReasonTooltip.Contains(TEXT("存在同优先级映射歧义")));
    TestFalse(TEXT("不把未命中的映射称为命中依据"), Ambiguous.Rows[0]->ReasonTooltip.Contains(TEXT("e:/alternative")));

    Index.Records.Reset();
    Index.BySource.Reset();
    auto Unknown = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Unknown->ObjectPath = FSoftObjectPath(TEXT("/Game/Details/Unknown.Unknown"));
    Index.UnknownRecords.Add(Unknown);
    const FSearchOutput UnknownResult = Match(Index, {TEXT("C:/Missing.fbx")}, Settings, {}, Cancel);
    if (!TestEqual(TEXT("未知来源行仍被显示"), UnknownResult.Rows.Num(), 1)) { return false; }
    TestEqual(TEXT("未知来源 Tooltip 说明而非虚构匹配"), UnknownResult.Rows[0]->ReasonTooltip, UnknownResult.Rows[0]->Status);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantColumnConfigTest, "ImportAssistant.Settings.ColumnVisibilityConfig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantColumnConfigTest::RunTest(const FString&)
{
    TStrongObjectPtr<UImportAssistantSettings> Settings(NewObject<UImportAssistantSettings>());
    TestTrue(TEXT("无列配置时默认全部显示"), Settings->HiddenResultColumns.IsEmpty());
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ImportAssistantTests") /
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Filename = Directory / TEXT("ColumnVisibility.ini");
    Settings->HiddenResultColumns = {TEXT("Source"), TEXT("AutoReimport"), TEXT("Dirty"), TEXT("VersionTime"), TEXT("State")};
    FString Error;
    if (!TestTrue(TEXT("保存列显隐到独立测试 Config"), Settings->SaveToFile(Filename, Error))) { AddError(Error); return false; }
    TStrongObjectPtr<UImportAssistantSettings> Loaded(NewObject<UImportAssistantSettings>());
    Loaded->HiddenResultColumns = {TEXT("Type")};
    if (!TestTrue(TEXT("重新读取列显隐"), Loaded->LoadFromFile(Filename, Error))) { AddError(Error); return false; }
    TestTrue(TEXT("配置覆盖旧数组且保留隐藏信息列"), Loaded->HiddenResultColumns == Settings->HiddenResultColumns);
    TestTrue(TEXT("显隐不改变检索规则"), Loaded->bMatchPath == Settings->bMatchPath && Loaded->bRecursive == Settings->bRecursive);
    Loaded->HiddenResultColumns.Reset();
    TestTrue(TEXT("恢复默认可以写入空隐藏数组"), Loaded->SaveToFile(Filename, Error));
    TestTrue(TEXT("重新载入空隐藏数组"), Settings->LoadFromFile(Filename, Error));
    TestTrue(TEXT("恢复默认不会残留上次隐藏项"), Settings->HiddenResultColumns.IsEmpty());
    const FString LegacyFilename = Directory / TEXT("BeforeColumns.ini");
    TestTrue(TEXT("写入不含列设置的旧版本配置"), FFileHelper::SaveStringToFile(TEXT("[/Script/ImportAssistant.ImportAssistantSettings]\n"
        "AssetDirectories=(Path=\"/Game\")\nbMatchPath=True\nbMatchKeyword=False\n"), *LegacyFilename));
    Settings->HiddenResultColumns = {TEXT("Path")};
    TestTrue(TEXT("旧版本配置仍可读取"), Settings->LoadFromFile(LegacyFilename, Error));
    TestTrue(TEXT("旧版本配置没有列字段时按默认显示全部"), Settings->HiddenResultColumns.IsEmpty());
    const FArrayProperty* Property = FindFProperty<FArrayProperty>(Settings->GetClass(), GET_MEMBER_NAME_CHECKED(UImportAssistantSettings, HiddenResultColumns));
    if (TestNotNull(TEXT("列设置为 UE Config 属性"), Property))
    {
        TestTrue(TEXT("列设置持久化且不重复暴露在左侧检索面板"), Property->HasAnyPropertyFlags(CPF_Config) && !Property->HasAnyPropertyFlags(CPF_Edit));
    }
    AddInfo(TEXT("列配置测试文件保留于：") + Directory);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantVersionTimeTest, "ImportAssistant.Matching.ImportedVersionTime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantVersionTimeTest::RunTest(const FString&)
{
    const FDateTime First(2026, 11, 8, 12, 34, 56);
    const FDateTime Second(2026, 11, 9, 3, 4, 5);
    FAssetImportInfo Info;
    Info.SourceFiles.Emplace(TEXT("C:/MissingSource/First.fbx"), First);
    Info.SourceFiles.Emplace(TEXT("C:/MissingSource/Second.fbx"), Second);
    Info.SourceFiles.Emplace(TEXT("C:/MissingSource/Unknown.fbx"));
    FAssetDataTagMap Tags;
    Tags.Add(UObject::SourceFileTagName(), Info.ToJson());
    const FAssetData Data(FName(TEXT("/Game/Version/Run")), FName(TEXT("/Game/Version")), FName(TEXT("Run")),
        UAnimSequence::StaticClass()->GetClassPathName(), MoveTemp(Tags));
    const auto Record = MakeRecord(Data, TEXT(""));
    if (!TestEqual(TEXT("时间与全部来源槽位一同保存"), Record->Sources.Num(), 3)) { return false; }
    TestEqual(TEXT("按 JSON 读取 UTC 原值，不访问缺失源文件"), Record->Sources[0].ImportedTimestampUtc, First);
    const auto Version = DescribeSourceVersion(*Record, 1);
    TestEqual(TEXT("本行按匹配槽位显示，而非取第一项或最大值"), Version.Label, FString(TEXT("2026-11-09 03:04:05 UTC")));
    TestTrue(TEXT("单元格只保留具体来源与时间，通用语义迁到表头"), !Version.Tooltip.Contains(TEXT("不是重导入操作时间")) &&
        Version.Tooltip.Contains(TEXT("First.fbx")) && Version.Tooltip.Contains(TEXT("Second.fbx")) && Version.Tooltip.Contains(TEXT("2026-11-08 12:34:56 UTC")));
    TestEqual(TEXT("缺省零时间不伪造为公元或 Unix 时间"), DescribeSourceVersion(*Record, 2).Label, FString(TEXT("未知")));
    TestTrue(TEXT("多来源但没有明确槽位时不猜测"), DescribeSourceVersion(*Record, INDEX_NONE).Label.Contains(TEXT("未确定槽位")));
    FAssetRecord Legacy;
    Legacy.Sources.Add({TEXT("C:/Legacy/Run.fbx"), {}});
    TestEqual(TEXT("旧来源记录没有时间时显示未知"), DescribeSourceVersion(Legacy, 0).Label, FString(TEXT("未知")));
    FAssetRecord Empty;
    TestEqual(TEXT("无来源记录显示未知"), DescribeSourceVersion(Empty, INDEX_NONE).Label, FString(TEXT("未知")));
    TestTrue(TEXT("纯元数据检查不加载资产"), Record->ObjectPath.ResolveObject() == nullptr);
    return true;
}
#endif









