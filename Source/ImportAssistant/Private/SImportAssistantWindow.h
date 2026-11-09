#pragma once

#include "ImportAssistantAssetIndex.h"
#include "ImportAssistantAutoReimport.h"
#include "ImportAssistantAssetPresentation.h"
#include "ImportAssistantReimportQueue.h"
#include "ImportAssistantResultGroups.h"
#include "ImportAssistantSettings.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class IDetailsView;
class SHeaderRow;
class SCheckBox;

class SImportAssistantWindow : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SImportAssistantWindow) : _LoadSavedSettings(true) {}
        SLATE_ARGUMENT(bool, LoadSavedSettings)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    ~SImportAssistantWindow();
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;
    virtual FReply OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event) override;
    virtual FReply OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event) override;
    bool CanClose() const { return !Queue.IsRunning() && !bPreparingUpdate; }

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FImportAssistantUpdateControlTest;
    friend class FImportAssistantSelectionCommand;
    friend class FImportAssistantColumnVisibilityTest;
    friend class FImportAssistantAutoReimportRefreshTest;
    friend class FImportAssistantVersionRefreshTest;
    friend class FImportAssistantColumnTooltipTest;
#endif
    void AddFiles(const TArray<FString>& Added);
    FReply Browse();
    FReply Search();
    FReply Execute();
    FReply ToggleUpdate();
    FText UpdateButtonText() const;
    FText UpdateButtonTooltip() const;
    bool CanToggleUpdate() const;
    FReply Export();
    TSharedRef<SWidget> BuildColumnMenu();
    bool IsResultColumnVisible(FName ColumnId) const;
    void ToggleResultColumn(FName ColumnId);
    void ResetResultColumns();
    void ApplyResultColumnVisibility();
    void SaveSettings();
    TSharedRef<SCheckBox> CreateSelectionCheckBox(ImportAssistant::FResultNodePtr Group = nullptr);
    ECheckBoxState GetSelectionState(ImportAssistant::FResultNodePtr Group = nullptr) const;
    bool CanChangeSelection(ImportAssistant::FResultNodePtr Group = nullptr) const;
    void ToggleSelection(ImportAssistant::FResultNodePtr Group = nullptr);
    void SetSelected(ImportAssistant::FRowPtr Row, bool bSelected);
    bool CanSelectResults() const;
    bool CanExecute() const;
    TSharedRef<ITableRow> GenerateRow(ImportAssistant::FResultNodePtr Node, const TSharedRef<STableViewBase>& Owner);
    void GetResultChildren(ImportAssistant::FResultNodePtr Node, TArray<ImportAssistant::FResultNodePtr>& OutChildren) const;
    void RebuildGroups(bool bPending);
    void CheckTemplateDirectories();
    void OnAutoReimportSettingsChanged(FName PropertyName);
    void RefreshAutoReimport(bool bForce);
    void UpdateAutoReimportRows();
    void RefreshDirtyAssets();
    void RefreshVersionTimes();
    void OnSettingsEdited(const FPropertyChangedEvent& Event);
    FText StatusText() const;
    void SettingsChanged();

    TStrongObjectPtr<UImportAssistantSettings> Settings;
    TSharedPtr<IDetailsView> SettingsView;
    bool bUseSavedSettings = true;
    FString ConfigError;
    TUniquePtr<ImportAssistant::FAssetIndex> Index;
    ImportAssistant::FReimportQueue Queue;
    ImportAssistant::FAssetPresentationCache AssetPresentation;
    ImportAssistant::FAutoReimportSnapshot AutoReimportSnapshot;
    TMap<FString, ImportAssistant::FAutoReimportInfo> AutoReimportFiles;
    TOptional<TArray<FString>> LastMonitoredDirectories;
    FDelegateHandle AutoReimportSettingsHandle;
    FDelegateHandle DirtyStateHandle;
    uint64 AutoReimportAssetRevision = MAX_uint64;
    double NextAutoReimportCheck = 0;
    double NextDirtyCheck = 0;
    uint64 DirtyAssetRevision = MAX_uint64;
    uint64 VersionAssetRevision = MAX_uint64;
    bool bDirtyStatusChanged = true;
    bool bPreparingUpdate = false;
    double AutoReimportSettingsSettleUntil = 0;
    bool bAutoReimportSettingsDirty = true;
    TArray<FString> Files;
    TArray<ImportAssistant::FFileSearchScope> FileScopes;
    bool bCheckTemplateDirectories = false;
    TArray<ImportAssistant::FPathMapping> Mappings;
    TArray<FName> ActiveRoots;
    bool bActiveRecursive = true;
    TArray<ImportAssistant::FRowPtr> Rows;
    TArray<ImportAssistant::FResultNodePtr> Groups;
    TSharedPtr<STreeView<ImportAssistant::FResultNodePtr>> Tree;
    TSharedPtr<SHeaderRow> ResultHeader;
    TFuture<ImportAssistant::FSearchOutput> SearchTask;
    TSharedPtr<std::atomic_bool, ESPMode::ThreadSafe> CancelSearch;
    uint64 TaskRevision = 0;
    uint64 ResultRevision = MAX_uint64;
    bool bSearchRequested = false;
    bool bSettingsDirty = true;
    bool bShowReport = false;
    bool bWasRunning = false;
    FString Notice;
    int32 SelectedCount = 0;
    int32 SelectableCount = 0;
    TMap<FString, ImportAssistant::FResultNodePtr> SelectionGroups;
    TMap<FSoftObjectPath, ImportAssistant::FRowPtr> SelectedAssets;
    int32 UnknownCount = 0;
    TArray<FString> UnmatchedFiles;
    double SearchSeconds = 0;
};





