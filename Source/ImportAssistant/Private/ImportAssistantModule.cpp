#include "Modules/ModuleManager.h"

#include "Framework/Docking/TabManager.h"
#include "SImportAssistantWindow.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

class FImportAssistantModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (IsRunningCommandlet()) { return; }
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TEXT("ImportAssistant"), FOnSpawnTab::CreateRaw(this, &FImportAssistantModule::SpawnTab))
            .SetDisplayName(FText::FromString(TEXT("FBX 资产导入助手"))).SetMenuType(ETabSpawnerMenuType::Hidden);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FImportAssistantModule::RegisterMenu));
    }

    virtual void ShutdownModule() override
    {
        if (IsRunningCommandlet()) { return; }
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TEXT("ImportAssistant"));
        if (TSharedPtr<SDockTab> Tab = OpenTab.Pin()) { Tab->RequestCloseTab(); }
    }

    virtual bool SupportsDynamicReloading() override { return false; }

private:
    void RegisterMenu()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* MainMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu"));
        MainMenu->FindOrAddSection(NAME_None).AddSubMenu(TEXT("FBX"), FText::FromString(TEXT("FBX 工具")),
            FText::FromString(TEXT("FBX 资产工具")), FNewToolMenuChoice());
        UToolMenu* Menu = UToolMenus::Get()->RegisterMenu(TEXT("LevelEditor.MainMenu.FBX"));
        Menu->FindOrAddSection(TEXT("ImportAssistant")).AddMenuEntry(TEXT("OpenImportAssistant"), FText::FromString(TEXT("资产导入助手")),
            FText::FromString(TEXT("检索同源 FBX 动画、静态模型和骨骼模型并选择性更新")), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]
            {
                FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("ImportAssistant")));
            })));
    }

    TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs&)
    {
        TSharedRef<SImportAssistantWindow> Window = SNew(SImportAssistantWindow);
        TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab)[Window];
        Tab->SetCanCloseTab(SDockTab::FCanCloseTab::CreateLambda([Weak = TWeakPtr<SImportAssistantWindow>(Window)]
        {
            const auto Pinned = Weak.Pin();
            return !Pinned || Pinned->CanClose();
        }));
        OpenTab = Tab;
        return Tab;
    }

    TWeakPtr<SDockTab> OpenTab;
};

IMPLEMENT_MODULE(FImportAssistantModule, ImportAssistant)
