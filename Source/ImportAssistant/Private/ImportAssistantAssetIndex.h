#pragma once

#include "ImportAssistantMatcher.h"
#include "Async/Future.h"
#include "AssetRegistry/ARFilter.h"
#include "HAL/CriticalSection.h"

class IAssetRegistry;

namespace ImportAssistant
{
class FAssetIndex
{
public:
    FAssetIndex();
    ~FAssetIndex();
    void Configure(const TArray<FName>& Roots, bool bRecursive);
    void Tick();
    void RefreshMemory();
    bool IsReady() const;
    FString GetStatus() const;
    uint64 GetRevision() const;
    TSharedRef<const FSearchSnapshot, ESPMode::ThreadSafe> Snapshot() const;
    int32 Num() const { return Records.Num(); }

private:
    struct FChanges
    {
        FCriticalSection Mutex;
        TSet<FSoftObjectPath> Paths;
        std::atomic<uint64> Revision{0};
        void Add(const FSoftObjectPath& Path);
    };

    void Replace(FRecordPtr Record);
    void Remove(const FSoftObjectPath& Path);
    bool InScope(const FAssetData& Data) const;
    IAssetRegistry* Registry;
    FARFilter Filter;
    FString DataSourceDirectory;
    TSharedRef<FChanges, ESPMode::ThreadSafe> Changes;
    TMap<FSoftObjectPath, FRecordPtr> Records;
    TMap<FString, TArray<FRecordPtr>> BySource;
    struct FBuiltIndex
    {
        TMap<FSoftObjectPath, FRecordPtr> Records;
        TMap<FString, TArray<FRecordPtr>> BySource;
    };
    TFuture<FBuiltIndex> Build;
    TArray<FAssetData> PendingMemory;
    int32 MemoryCursor = 0;
    mutable TSharedPtr<const FSearchSnapshot, ESPMode::ThreadSafe> CachedSnapshot;
    bool bRebuild = true;
    bool bConfigured = false;
    FDelegateHandle Added, Removed, Renamed, Updated, UpdatedOnDisk, SourceChanged, Mounted, Unmounted;
    FDelegateHandle MemoryCreated, MemoryDeleted, ObjectChanged, AssetLoaded, Transacted, PackageDirtied;
};
}



