#include "SImportAssistantWindow.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Views/SHeaderRow.h"

using namespace ImportAssistant;

enum class ESelectionScenario : uint8
{
    TriState,
    Conflicts,
    Availability,
    Report,
    Diagnostics
};

class FImportAssistantSelectionCommand : public IAutomationLatentCommand
{
public:
    FImportAssistantSelectionCommand(FAutomationTestBase* InTest, ESelectionScenario InScenario)
        : Test(InTest), Scenario(InScenario), Start(FPlatformTime::Seconds()), Window(SNew(SImportAssistantWindow).LoadSavedSettings(false))
    {
        // 仅建立空测试范围的索引就绪状态；候选由值记录构造，不创建或加载生产动画。
        const FString Root = TEXT("/Game/__ImportAssistantSelection_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Window->Index->Configure({FName(Root)}, true);
    }

    virtual bool Update() override
    {
        Window->Index->Tick();
        if (!Window->Index->IsReady())
        {
            if (FPlatformTime::Seconds() - Start > 30) { Test->AddError(TEXT("三态勾选测试的空范围索引未能就绪")); return true; }
            return false;
        }
        Window->Files = {TEXT("C:/Selection/A.fbx"), TEXT("C:/Selection/B.fbx"), TEXT("C:/Selection/Empty.fbx")};
        Window->bSettingsDirty = false;
        Window->bSearchRequested = false;
        Window->ResultRevision = Window->Index->GetRevision();
        switch (Scenario)
        {
        case ESelectionScenario::TriState: RunTriState(); break;
        case ESelectionScenario::Conflicts: RunConflicts(); break;
        case ESelectionScenario::Availability: RunAvailability(); break;
        case ESelectionScenario::Report: RunReport(); break;
        case ESelectionScenario::Diagnostics: RunDiagnostics(); break;
        }
        return true;
    }

private:
    FRowPtr MakeRow(const TCHAR* Name, const FString& File, EMatchReason Reason = EMatchReason::Exact)
    {
        auto Asset = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
        Asset->Name = Name;
        Asset->AssetClassPath = FTopLevelAssetPath(TEXT("/Script/Engine.AnimSequence"));
        Asset->ObjectPath = FSoftObjectPath(FString::Printf(TEXT("/Game/__Selection/%s.%s"), Name, Name));
        Asset->Sources.Add({File, {}});
        FRowPtr Row = MakeShared<FMatchRow>();
        Row->Asset = Asset;
        Row->File = File;
        Row->SourceIndex = 0;
        Row->Reason = Reason;
        Row->Status = TEXT("未选择");
        return Row;
    }

    TSharedRef<SCheckBox> HeaderCheckBox()
    {
        const auto& Columns = Window->ResultHeader->GetColumns();
        return StaticCastSharedRef<SCheckBox>(Columns[Window->ResultHeader->FindColumnIndex(TEXT("Select"))].HeaderContent.Widget);
    }

    TSharedPtr<SCheckBox> FindGroupCheckBox(TSharedRef<SWidget> Widget)
    {
        if (Widget->GetTag() == FName(TEXT("ImportAssistant.GroupSelection"))) { return StaticCastSharedRef<SCheckBox>(Widget); }
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            if (TSharedPtr<SCheckBox> Found = FindGroupCheckBox(Children->GetChildAt(Index))) { return Found; }
        }
        return nullptr;
    }

    TSharedPtr<SCheckBox> GroupCheckBox(FResultNodePtr Group)
    {
        const TSharedRef<ITableRow> Row = Window->GenerateRow(Group, Window->Tree.ToSharedRef());
        const auto Box = FindGroupCheckBox(Row->AsWidget());
        Test->TestTrue(TEXT("真实组标题存在三态勾选框"), Box.IsValid());
        return Box;
    }

    void RunTriState()
    {
        const FRowPtr A1 = MakeRow(TEXT("A1"), Window->Files[0]);
        const FRowPtr A2 = MakeRow(TEXT("A2"), Window->Files[0], EMatchReason::Keyword);
        const FRowPtr B = MakeRow(TEXT("B"), Window->Files[1], EMatchReason::Mapping);
        const FRowPtr Invalid = MakeRow(TEXT("Invalid"), Window->Files[0]);
        Invalid->bSourceAmbiguous = true;
        const FRowPtr Unknown = MakeRow(TEXT("Unknown"), FString());
        Window->Rows = {A1, A2, B, Invalid, Unknown};
        Window->RebuildGroups(false);
        Window->Tree->SetItemExpansion(Window->Groups[0], false);
        const auto Header = HeaderCheckBox();
        const auto ABox = GroupCheckBox(Window->Groups[0]);
        const auto BBox = GroupCheckBox(Window->Groups[1]);
        if (!ABox || !BBox) { return; }
        Test->TestTrue(TEXT("表头文字已由实际勾选框替代"), Header->GetTag() == FName(TEXT("ImportAssistant.GlobalSelection")));
        Test->TestEqual(TEXT("初次检索默认没有选择"), Header->GetCheckedState(), ECheckBoxState::Unchecked);
        Test->TestEqual(TEXT("全局只统计可选行"), Window->SelectableCount, 3);
        Test->TestEqual(TEXT("组内不统计歧义来源"), Window->Groups[0]->SelectableCount, 2);
        Test->TestTrue(TEXT("有效结果全局与组复选框可用"), Header->IsEnabled() && ABox->IsEnabled());
        ABox->ToggleCheckedState();
        Test->TestTrue(TEXT("组内全选包含关键词但不选择其他组和无效行"), A1->bSelected && A2->bSelected && !B->bSelected && !Invalid->bSelected);
        Test->TestEqual(TEXT("组内全选状态"), ABox->GetCheckedState(), ECheckBoxState::Checked);
        Test->TestEqual(TEXT("分组选择同步为全局横杠"), Header->GetCheckedState(), ECheckBoxState::Undetermined);
        Test->TestFalse(TEXT("折叠组的勾选操作不会展开组"), Window->Tree->IsItemExpanded(Window->Groups[0]));
        Header->ToggleCheckedState();
        Test->TestEqual(TEXT("全局横杠点击取消而非补齐选择"), Window->SelectedCount, 0);
        Header->ToggleCheckedState();
        Test->TestTrue(TEXT("全局全选覆盖所有分组及映射候选"), A1->bSelected && A2->bSelected && B->bSelected);
        Test->TestEqual(TEXT("全局全部选中"), Header->GetCheckedState(), ECheckBoxState::Checked);
        Test->TestEqual(TEXT("全局全选反映到另一个组"), BBox->GetCheckedState(), ECheckBoxState::Checked);
        Window->SetSelected(A2, false);
        Test->TestEqual(TEXT("逐行取消使组变为横杠"), ABox->GetCheckedState(), ECheckBoxState::Undetermined);
        Test->TestEqual(TEXT("逐行取消使全局变为横杠"), Header->GetCheckedState(), ECheckBoxState::Undetermined);
        ABox->ToggleCheckedState();
        Test->TestTrue(TEXT("组内横杠点击只取消本组"), !A1->bSelected && !A2->bSelected && B->bSelected);
        BBox->ToggleCheckedState();
        Test->TestEqual(TEXT("组内全部选中点击取消并联动全局未选"), Header->GetCheckedState(), ECheckBoxState::Unchecked);
        Header->ToggleCheckedState();
        Header->ToggleCheckedState();
        Test->TestEqual(TEXT("全局勾选状态点击取消全部"), Header->GetCheckedState(), ECheckBoxState::Unchecked);
        Window->SetSelected(A1, true);
        Window->SetSelected(A1, true);
        Test->TestEqual(TEXT("重复选择不会重复累计"), Window->SelectedCount, 1);
        Test->TestEqual(TEXT("组统计同样保持一致"), Window->Groups[0]->SelectedCount, 1);
        Window->RebuildGroups(false);
        Test->TestEqual(TEXT("监控或结果分组重建后重新汇总选择"), Header->GetCheckedState(), ECheckBoxState::Undetermined);
        Test->TestEqual(TEXT("重建后的新组仍为部分已选"), Window->GetSelectionState(Window->Groups[0]), ECheckBoxState::Undetermined);
        Window->SetSelected(A2, true);
        Test->TestEqual(TEXT("重建后选择更新新组计数"), Window->GetSelectionState(Window->Groups[0]), ECheckBoxState::Checked);
        Test->TestFalse(TEXT("重建分组不丢失原折叠状态"), Window->Tree->IsItemExpanded(Window->Groups[0]));
        Window->ToggleResultColumn(TEXT("State"));
        Test->TestEqual(TEXT("隐藏状态列不影响三态统计"), Header->GetCheckedState(), ECheckBoxState::Undetermined);
        const auto ClickBox = GroupCheckBox(Window->Groups[0]);
        if (ClickBox)
        {
            const FPointerEvent Mouse(0, FVector2D(8, 8), FVector2D(8, 8), TSet<FKey>{EKeys::LeftMouseButton},
                EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
            Test->TestTrue(TEXT("复选框处理鼠标按下，不向组行传播折叠事件"), ClickBox->OnMouseButtonDown(
                FGeometry::MakeRoot(FVector2f(20, 20), FSlateLayoutTransform()), Mouse).IsEventHandled());
        }
    }

    void RunDiagnostics()
    {
        Window->Files = {TEXT("C:/Diagnostics/Hero_Run.fbx"), TEXT("C:/Diagnostics/Hero_Walk.fbx"), TEXT("C:/Diagnostics/Empty.fbx")};
        FSearchSnapshot Snapshot;
        for (const FString& Name : {FString(TEXT("Hero_Run_Candidate")), FString(TEXT("Unrelated"))})
        {
            auto Asset = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
            Asset->Name = Name;
            Asset->AssetClassPath = FTopLevelAssetPath(TEXT("/Script/Engine.SkeletalMesh"));
            Asset->PackageName = TEXT("/Game/__Diagnostics/") + Name;
            Asset->ObjectPath = FSoftObjectPath(Asset->PackageName + TEXT(".") + Name);
            Snapshot.Records.Add(Asset);
            Snapshot.UnknownRecords.Add(Asset);
        }
        FMatchSettings Settings;
        Settings.bMatchKeyword = true;
        std::atomic_bool Cancel(false);
        const FSearchOutput Output = Match(Snapshot, Window->Files, Settings, {}, Cancel);
        Window->Rows = Output.Rows;
        Window->UnmatchedFiles = Output.UnmatchedFiles;
        Window->UnmatchedUnknownCount = Output.UnmatchedUnknownCount;
        Window->RebuildGroups(false);
        if (!Test->TestEqual(TEXT("实际窗口只接收关键词命中的来源未知候选"), Window->Rows.Num(), 1)) { return; }
        Test->TestEqual(TEXT("窗口不建立来源未知额外组"), Window->Groups.Num(), Window->Files.Num());
        Test->TestTrue(TEXT("候选归入实际命中的 FBX"), Window->Groups[0]->Children[0]->Row == Window->Rows[0]);
        const FString Status = Window->StatusText().ToString();
        Test->TestTrue(TEXT("状态栏匹配数量排除范围诊断"), Status.Contains(TEXT("匹配结果 1 行")));
        Test->TestTrue(TEXT("独立提示无关未知来源的数量及边界"), Status.Contains(TEXT("另有 1 个资产缺少可用来源记录")) &&
            Status.Contains(TEXT("未确认与本次 FBX 有关；不计入匹配结果")));
        HeaderCheckBox()->ToggleCheckedState();
        Test->TestTrue(TEXT("表头全选不能选中缺来源槽位候选"), Window->SelectedCount == 0 && !Window->Rows[0]->bSelected);
        Window->UnmatchedUnknownCount = 0;
        Test->TestFalse(TEXT("没有额外未知来源时不保留旧提示"), Window->StatusText().ToString().Contains(TEXT("另有")));
    }

    void RunConflicts()
    {
        const FRowPtr Unique = MakeRow(TEXT("Unique"), Window->Files[0]);
        const FRowPtr A = MakeRow(TEXT("Shared"), Window->Files[0]);
        const FRowPtr B = MakeRow(TEXT("Shared"), Window->Files[1]);
        A->bFileConflict = true;
        B->bFileConflict = true;
        Window->Rows = {Unique, A, B};
        Window->RebuildGroups(false);
        const auto Header = HeaderCheckBox();
        const auto ABox = GroupCheckBox(Window->Groups[0]);
        const auto BBox = GroupCheckBox(Window->Groups[1]);
        if (!ABox || !BBox) { return; }
        Header->ToggleCheckedState();
        Test->TestTrue(TEXT("全局全选跳过互斥来源，不按遍历顺序选最后一个"), Unique->bSelected && !A->bSelected && !B->bSelected);
        Test->TestEqual(TEXT("冲突行未选时全局保持部分已选"), Header->GetCheckedState(), ECheckBoxState::Undetermined);
        Test->TestTrue(TEXT("提示按去重资产计数，要求明确来源"), Window->Notice.Contains(TEXT("1 个存在多 FBX 来源冲突")));
        Header->ToggleCheckedState();
        ABox->ToggleCheckedState();
        Test->TestTrue(TEXT("组内全选明确选择本组冲突来源"), A->bSelected && Unique->bSelected && !B->bSelected);
        BBox->ToggleCheckedState();
        Test->TestTrue(TEXT("选择另一组来源自动取消原冲突行但不影响其他资产"), !A->bSelected && B->bSelected && Unique->bSelected);
        Test->TestEqual(TEXT("互斥切换后原组状态变为横杠"), ABox->GetCheckedState(), ECheckBoxState::Undetermined);
        Test->TestEqual(TEXT("新来源所在组为勾选"), BBox->GetCheckedState(), ECheckBoxState::Checked);
        Test->TestEqual(TEXT("互斥切换后全局计数没有重复"), Window->SelectedCount, 2);
        Test->TestEqual(TEXT("已选对象索引按资产去重"), Window->SelectedAssets.Num(), 2);
        ABox->ToggleCheckedState();
        Test->TestTrue(TEXT("原组横杠取消不能移除另一组所选来源"), !Unique->bSelected && B->bSelected);
        Window->SetSelected(A, true);
        Test->TestTrue(TEXT("逐行互斥选择同样更新两组状态"), A->bSelected && !B->bSelected && BBox->GetCheckedState() == ECheckBoxState::Unchecked);
        Test->TestEqual(TEXT("逐行冲突处理保留反馈"), B->Status, FString(TEXT("已选择另一 FBX 来源")));
        Window->RebuildGroups(false);
        Window->SetSelected(B, true);
        Test->TestTrue(TEXT("分组重建后互斥索引仍有效"), B->bSelected && !A->bSelected && Window->SelectedCount == 1);
        Header->ToggleCheckedState();
        Test->TestEqual(TEXT("冲突导致横杠时全局可全部取消"), Window->SelectedCount, 0);
        Window->Rows = {A, B};
        Window->RebuildGroups(false);
        Header->ToggleCheckedState();
        Test->TestEqual(TEXT("只有冲突项时不能伪造全部选中"), Header->GetCheckedState(), ECheckBoxState::Unchecked);
        Test->TestTrue(TEXT("只有冲突项时仍明确提示"), Window->Notice.Contains(TEXT("跳过 1 个")));
    }

    void RunAvailability()
    {
        const FRowPtr Item = MakeRow(TEXT("Ready"), Window->Files[0]);
        const FRowPtr Invalid = MakeRow(TEXT("Invalid"), Window->Files[1]);
        Invalid->SourceIndex = INDEX_NONE;
        Window->Rows = {Item, Invalid};
        Window->RebuildGroups(false);
        const auto Header = HeaderCheckBox();
        const auto Box = GroupCheckBox(Window->Groups[0]);
        const auto InvalidBox = GroupCheckBox(Window->Groups[1]);
        const auto EmptyBox = GroupCheckBox(Window->Groups[2]);
        if (!Box || !InvalidBox || !EmptyBox) { return; }
        // IsEnabled 是 Slate 属性缓存，独立构造的测试控件需先做预布局才能读取最新绑定值。
        auto RefreshEnabled = [&]
        {
            Header->SlatePrepass();
            Box->SlatePrepass();
            InvalidBox->SlatePrepass();
            EmptyBox->SlatePrepass();
        };
        RefreshEnabled();
        Test->TestFalse(TEXT("来源无效的组不可全选"), InvalidBox->IsEnabled());
        Test->TestFalse(TEXT("零匹配组不可全选"), EmptyBox->IsEnabled());
        Test->TestEqual(TEXT("零匹配仍为未勾选"), EmptyBox->GetCheckedState(), ECheckBoxState::Unchecked);
        Window->bSettingsDirty = true;
        RefreshEnabled();
        Header->ToggleCheckedState();
        Test->TestTrue(TEXT("配置变更后禁用勾选且拒绝回调"), !Header->IsEnabled() && !Box->IsEnabled() && !Item->bSelected);
        Window->bSettingsDirty = false;
        Window->bSearchRequested = true;
        RefreshEnabled();
        Box->ToggleCheckedState();
        Test->TestTrue(TEXT("检索请求未完成时禁用勾选"), !Box->IsEnabled() && !Item->bSelected);
        Window->bSearchRequested = false;
        Window->ResultRevision = MAX_uint64;
        RefreshEnabled();
        Header->ToggleCheckedState();
        Test->TestTrue(TEXT("过期结果不接受选择"), !Header->IsEnabled() && !Item->bSelected);
        Window->ResultRevision = Window->Index->GetRevision();
        RefreshEnabled();
        Header->ToggleCheckedState();
        Test->TestTrue(TEXT("有效结果恢复选择"), Item->bSelected);
        Window->Queue.Start({Item});
        RefreshEnabled();
        Header->ToggleCheckedState();
        Test->TestTrue(TEXT("正在更新时禁用且不能取消批次选择"), !Header->IsEnabled() && !Box->IsEnabled() && Item->bSelected);
        Window->Queue.Cancel();
        Window->Queue.Tick();
        Window->bShowReport = true;
        RefreshEnabled();
        Box->ToggleCheckedState();
        Test->TestTrue(TEXT("报告保留时恢复选择并允许取消"), Box->IsEnabled() && !Item->bSelected);
        Window->bShowReport = false;
        RefreshEnabled();
        Window->Rows.Reset();
        Window->RebuildGroups(false);
        RefreshEnabled();
        Test->TestTrue(TEXT("清空结果后复选框禁用并回到未选"), !Header->IsEnabled() && Header->GetCheckedState() == ECheckBoxState::Unchecked);
        Test->TestTrue(TEXT("清空结果后移除全部选择索引"), Window->SelectedAssets.IsEmpty() && Window->SelectedCount == 0 && Window->SelectableCount == 0);
    }

    void RunReport()
    {
        const FRowPtr A = MakeRow(TEXT("Report"), Window->Files[0]);
        const FRowPtr B = MakeRow(TEXT("Report"), Window->Files[1]);
        A->bFileConflict = true;
        B->bFileConflict = true;
        A->bSelected = true;
        A->Status = TEXT("已更新，未保存");
        B->Status = TEXT("未执行");
        Window->Rows = {A, B};
        Window->RebuildGroups(false);
        Window->Queue.Start({A});
        Window->Queue.Cancel();
        Window->bWasRunning = true;
        Window->Tick(FGeometry(), FPlatformTime::Seconds(), 0);
        Test->TestTrue(TEXT("真实 Tick 在批次结束后进入报告而非永久禁用状态"), Window->bShowReport && !Window->Queue.IsRunning());
        A->Status = TEXT("已更新，未保存");
        Window->ResultRevision = MAX_uint64;
        const auto Header = HeaderCheckBox();
        const auto ABox = GroupCheckBox(Window->Groups[0]);
        const auto BBox = GroupCheckBox(Window->Groups[1]);
        if (!ABox || !BBox) { return; }
        Header->SlatePrepass(); ABox->SlatePrepass(); BBox->SlatePrepass();
        Test->TestTrue(TEXT("报告不再因上批索引版本禁用选择"), Header->IsEnabled() && ABox->IsEnabled() && BBox->IsEnabled());
        Test->TestTrue(TEXT("已选资产可以进入新一批预检"), Window->CanExecute());
        BBox->ToggleCheckedState();
        Test->TestTrue(TEXT("报告内组选择仍互斥"), B->bSelected && !A->bSelected);
        Test->TestEqual(TEXT("互斥取消不覆盖上一批成功结果"), A->Status, FString(TEXT("已更新，未保存")));
        Test->TestEqual(TEXT("重新勾选未执行行不覆盖其结果"), B->Status, FString(TEXT("未执行")));
        Header->ToggleCheckedState();
        Test->TestFalse(TEXT("无选择时不允许执行"), Window->CanExecute());
        Test->TestTrue(TEXT("取消全部后选择入口保持可用"), Window->CanSelectResults());
        Window->SetSelected(A, true);
        Window->bPreparingUpdate = true;
        Header->SlatePrepass();
        Test->TestFalse(TEXT("新批预检及确认期间禁止再改选择"), Header->IsEnabled());
        Window->bPreparingUpdate = false;
        Window->bSettingsDirty = true;
        Test->TestFalse(TEXT("报告不能绕过检索配置变更"), Window->CanSelectResults());
        Window->bSettingsDirty = false;
        Window->bSearchRequested = true;
        Test->TestFalse(TEXT("报告不能绕过未完成的检索"), Window->CanSelectResults());
        Window->bSearchRequested = false;
        Window->Rows.Reset();
        Window->RebuildGroups(false);
        Test->TestEqual(TEXT("清空不残留三态计数"), Window->SelectedCount, 0);
    }

    FAutomationTestBase* Test;
    ESelectionScenario Scenario;
    double Start;
    TSharedRef<SImportAssistantWindow> Window;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantSelectionTriStateTest, "ImportAssistant.Editor.Selection.TriStateAndScopes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantSelectionTriStateTest::RunTest(const FString&)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FImportAssistantSelectionCommand>(this, ESelectionScenario::TriState));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantSelectionConflictTest, "ImportAssistant.Editor.Selection.CrossGroupConflicts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantSelectionConflictTest::RunTest(const FString&)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FImportAssistantSelectionCommand>(this, ESelectionScenario::Conflicts));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantSelectionAvailabilityTest, "ImportAssistant.Editor.Selection.Availability",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantSelectionAvailabilityTest::RunTest(const FString&)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FImportAssistantSelectionCommand>(this, ESelectionScenario::Availability));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantSelectionReportTest, "ImportAssistant.Editor.Selection.ReportCanReselect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantSelectionReportTest::RunTest(const FString&)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FImportAssistantSelectionCommand>(this, ESelectionScenario::Report));
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportAssistantUnknownDiagnosticsTest, "ImportAssistant.Editor.UnknownSourceDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportAssistantUnknownDiagnosticsTest::RunTest(const FString&)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FImportAssistantSelectionCommand>(this, ESelectionScenario::Diagnostics));
    return true;
}
#endif
