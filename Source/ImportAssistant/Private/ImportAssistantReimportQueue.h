#pragma once

#include "ImportAssistantAssetState.h"
#include "InterchangeManager.h"
#include "HAL/CriticalSection.h"
#include "UObject/StrongObjectPtr.h"

class UAssetImportData;
class UPackage;

namespace ImportAssistant
{
class FReimportQueue
{
public:
    ~FReimportQueue();
    void Start(const TArray<FRowPtr>& Rows, TSharedPtr<FReimportPlan> ApprovedPlan = nullptr);
    void Tick();
    void Cancel();
    bool IsRunning() const { return bRunning; }
    bool IsStopRequested() const { return bRunning && bCancel; }
    FString GetStatus() const;

private:
    void BeginItem();
    void FinishItem();
    void ReleaseItem();
    void EndBatch();

    TArray<FRowPtr> Items;
    int32 Next = 0;
    bool bRunning = false;
    bool bCancel = false;
    FString StopReason;
    bool bTrackingImport = false;
    bool bItemSucceeded = false;
    FRowPtr Current;
    TStrongObjectPtr<UObject> Asset;
    UE::Interchange::FAssetImportResultPtr Result;
    TSet<TWeakObjectPtr<UPackage>> DirtiedPackages;
    TSharedPtr<FReimportPlan> Plan;
    FCriticalSection DirtyMutex;
    FDelegateHandle DirtyHandle;
    TWeakObjectPtr<UClass> OriginalImportClass;
    FString OldSource;
    bool bChangedSource = false;
};
}

