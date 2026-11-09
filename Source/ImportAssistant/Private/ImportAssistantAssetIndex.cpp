#include "ImportAssistantAssetIndex.h"

#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "Editor/EditorPerProjectUserSettings.h"

namespace ImportAssistant
{
void FAssetIndex::FChanges::Add(const FSoftObjectPath& Path)
{
    FScopeLock Lock(&Mutex);
    Paths.Add(Path);
    ++Revision;
}

FAssetIndex::FAssetIndex() : Changes(MakeShared<FChanges, ESPMode::ThreadSafe>())
{
    Registry = &FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const auto State = Changes;
    const FTopLevelAssetPath AnimationClass = UAnimSequence::StaticClass()->GetClassPathName();
    auto Changed = [State, AnimationClass](const FAssetData& Data)
    {
        if (Data.AssetClassPath == AnimationClass) { State->Add(Data.GetSoftObjectPath()); }
    };
    Added = Registry->OnAssetAdded().AddLambda(Changed);
    Removed = Registry->OnAssetRemoved().AddLambda(Changed);
    Updated = Registry->OnAssetUpdated().AddLambda(Changed);
    UpdatedOnDisk = Registry->OnAssetUpdatedOnDisk().AddLambda(Changed);
    Renamed = Registry->OnAssetRenamed().AddLambda([State, AnimationClass](const FAssetData& Data, const FString& OldPath)
    {
        if (Data.AssetClassPath != AnimationClass) { return; }
        State->Add(FSoftObjectPath(OldPath));
        State->Add(Data.GetSoftObjectPath());
    });
    SourceChanged = UAssetImportData::OnImportDataChanged.AddLambda([State](const FAssetImportInfo&, const UAssetImportData* Data)
    {
        if (const UAnimSequence* Animation = Data->GetTypedOuter<UAnimSequence>()) { State->Add(FSoftObjectPath(Animation)); }
    });
    auto MemoryChanged = [State](UObject* Object)
    {
        if (const UAnimSequence* Animation = Cast<UAnimSequence>(Object)) { State->Add(FSoftObjectPath(Animation)); }
    };
    MemoryCreated = Registry->OnInMemoryAssetCreated().AddLambda(MemoryChanged);
    MemoryDeleted = Registry->OnInMemoryAssetDeleted().AddLambda(MemoryChanged);
    AssetLoaded = FCoreUObjectDelegates::OnAssetLoaded.AddLambda(MemoryChanged);
    ObjectChanged = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([State](UObject* Object, FPropertyChangedEvent&)
    {
        const UAnimSequence* Animation = Cast<UAnimSequence>(Object);
        if (!Animation) { Animation = Object->GetTypedOuter<UAnimSequence>(); }
        if (Animation) { State->Add(FSoftObjectPath(Animation)); }
    });
    Transacted = FCoreUObjectDelegates::OnObjectTransacted.AddLambda([MemoryChanged](UObject* Object, const FTransactionObjectEvent&)
    {
        if (UAnimSequence* Animation = Object->GetTypedOuter<UAnimSequence>()) { MemoryChanged(Animation); }
        else { MemoryChanged(Object); }
    });
    PackageDirtied = UPackage::PackageMarkedDirtyEvent.AddLambda([State](UPackage* Package, bool)
    {
        // 某些原生路径更新不广播来源事件，脏包事件补充这些内存状态。
        ForEachObjectWithOuter(Package, [State](UObject* Object)
        {
            if (const UAnimSequence* Animation = Cast<UAnimSequence>(Object)) { State->Add(FSoftObjectPath(Animation)); }
        }, EGetObjectsFlags::None);
    });
    Mounted = FPackageName::OnContentPathMounted().AddLambda([State](const FString&, const FString&) { State->Add(FSoftObjectPath()); });
    Unmounted = FPackageName::OnContentPathDismounted().AddLambda([State](const FString&, const FString&) { State->Add(FSoftObjectPath()); });
}

FAssetIndex::~FAssetIndex()
{
    Registry->OnAssetAdded().Remove(Added);
    Registry->OnAssetRemoved().Remove(Removed);
    Registry->OnAssetRenamed().Remove(Renamed);
    Registry->OnAssetUpdated().Remove(Updated);
    Registry->OnAssetUpdatedOnDisk().Remove(UpdatedOnDisk);
    UAssetImportData::OnImportDataChanged.Remove(SourceChanged);
    Registry->OnInMemoryAssetCreated().Remove(MemoryCreated);
    Registry->OnInMemoryAssetDeleted().Remove(MemoryDeleted);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ObjectChanged);
    FCoreUObjectDelegates::OnAssetLoaded.Remove(AssetLoaded);
    FCoreUObjectDelegates::OnObjectTransacted.Remove(Transacted);
    UPackage::PackageMarkedDirtyEvent.Remove(PackageDirtied);
    FPackageName::OnContentPathMounted().Remove(Mounted);
    FPackageName::OnContentPathDismounted().Remove(Unmounted);
    if (Build.IsValid()) { Build.Wait(); }
}

void FAssetIndex::Configure(const TArray<FName>& Roots, bool bRecursive)
{
    Filter = FARFilter();
    Filter.PackagePaths = Roots;
    Filter.bRecursivePaths = bRecursive;
    Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
    Filter.bIncludeOnlyOnDiskAssets = true;
    DataSourceDirectory = GetDefault<UEditorPerProjectUserSettings>()->DataSourceFolder.Path;
    PendingMemory.Reset();
    MemoryCursor = 0;
    bConfigured = true;
    bRebuild = true;
    ++Changes->Revision;
    Registry->SearchAllAssets(false);
}

void FAssetIndex::RefreshMemory()
{
    if (!bConfigured || Build.IsValid() || bRebuild) { return; }
    const FString CurrentSourceDirectory = GetDefault<UEditorPerProjectUserSettings>()->DataSourceFolder.Path;
    if (CurrentSourceDirectory != DataSourceDirectory)
    {
        DataSourceDirectory = CurrentSourceDirectory;
        bRebuild = true;
    }
    else
    {
        PendingMemory.Reset();
        Registry->GetInMemoryAssets(Filter, PendingMemory);
        MemoryCursor = 0;
    }
    ++Changes->Revision;
}

bool FAssetIndex::InScope(const FAssetData& Data) const
{
    if (!Data.IsValid() || Data.AssetClassPath != UAnimSequence::StaticClass()->GetClassPathName()) { return false; }
    const FString Path = Data.PackagePath.ToString();
    for (const FName Root : Filter.PackagePaths)
    {
        const FString RootPath = Root.ToString();
        if (Path == RootPath || (Filter.bRecursivePaths && Path.StartsWith(RootPath + TEXT("/")))) { return true; }
    }
    return false;
}

void FAssetIndex::Remove(const FSoftObjectPath& Path)
{
    CachedSnapshot.Reset();
    FRecordPtr Old;
    if (!Records.RemoveAndCopyValue(Path, Old)) { return; }
    for (const FSourceRecord& Source : Old->Sources)
    {
        for (const FString& Key : Source.Candidates)
        {
            if (TArray<FRecordPtr>* Entries = BySource.Find(Key))
            {
                Entries->Remove(Old);
                if (Entries->IsEmpty()) { BySource.Remove(Key); }
            }
        }
    }
}

void FAssetIndex::Replace(FRecordPtr Record)
{
    if (const FRecordPtr* Existing = Records.Find(Record->ObjectPath))
    {
        if ((*Existing)->SourceJson == Record->SourceJson && (*Existing)->Skeleton == Record->Skeleton &&
            (*Existing)->AssetClassPath == Record->AssetClassPath) { return; }
    }
    Remove(Record->ObjectPath);
    Records.Add(Record->ObjectPath, Record);
    for (const FSourceRecord& Source : Record->Sources)
    {
        for (const FString& Key : Source.Candidates) { BySource.FindOrAdd(Key).AddUnique(Record); }
    }
}

void FAssetIndex::Tick()
{
    if (!bConfigured) { return; }
    if (Build.IsValid())
    {
        if (!Build.IsReady()) { return; }
        FBuiltIndex Built = Build.Get();
        Build = {};
        if (!bRebuild)
        {
            CachedSnapshot.Reset();
            Records = MoveTemp(Built.Records);
            BySource = MoveTemp(Built.BySource);
            // 注册表的内存采集必须在主线程；来源解析与索引覆盖分帧处理。
            PendingMemory.Reset();
            Registry->GetInMemoryAssets(Filter, PendingMemory);
            MemoryCursor = 0;
            ++Changes->Revision;
        }
    }
    if (Registry->IsLoadingAssets()) { return; }
    if (bRebuild)
    {
        bRebuild = false;
        const FARFilter DiskFilter = Filter;
        const FString SourceDirectory = DataSourceDirectory;
        IAssetRegistry* AssetRegistry = Registry;
        Build = Async(EAsyncExecution::ThreadPool, [DiskFilter, SourceDirectory, AssetRegistry]()
        {
            TArray<FAssetData> Assets;
            AssetRegistry->GetAssets(DiskFilter, Assets);
            FBuiltIndex Result;
            Result.Records.Reserve(Assets.Num());
            for (const FAssetData& Data : Assets)
            {
                FRecordPtr Record = MakeRecord(Data, SourceDirectory);
                Result.Records.Add(Record->ObjectPath, Record);
                for (const FSourceRecord& Source : Record->Sources)
                {
                    for (const FString& Key : Source.Candidates) { Result.BySource.FindOrAdd(Key).AddUnique(Record); }
                }
            }
            return Result;
        });
        return;
    }
    const double MemoryStart = FPlatformTime::Seconds();
    while (MemoryCursor < PendingMemory.Num())
    {
        Replace(MakeRecord(PendingMemory[MemoryCursor++], DataSourceDirectory));
        if (FPlatformTime::Seconds() - MemoryStart > 0.003) { return; }
    }
    PendingMemory.Reset();
    MemoryCursor = 0;
    TSet<FSoftObjectPath> Pending;
    {
        FScopeLock Lock(&Changes->Mutex);
        Swap(Pending, Changes->Paths);
    }
    const double Start = FPlatformTime::Seconds();
    for (auto It = Pending.CreateIterator(); It; ++It)
    {
        const FSoftObjectPath Path = *It;
        if (Path.IsNull()) { bRebuild = true; }
        else
        {
            const FAssetData Data = Registry->GetAssetByObjectPath(Path);
            if (InScope(Data)) { Replace(MakeRecord(Data, DataSourceDirectory)); }
            else { Remove(Path); }
        }
        It.RemoveCurrent();
        if (FPlatformTime::Seconds() - Start > 0.003) { break; }
    }
    if (!Pending.IsEmpty())
    {
        FScopeLock Lock(&Changes->Mutex);
        Changes->Paths.Append(Pending);
    }
}

bool FAssetIndex::IsReady() const
{
    if (!bConfigured || bRebuild || Build.IsValid() || !PendingMemory.IsEmpty() || Registry->IsLoadingAssets()) { return false; }
    FScopeLock Lock(&Changes->Mutex);
    return Changes->Paths.IsEmpty();
}

uint64 FAssetIndex::GetRevision() const { return Changes->Revision.load(); }

FString FAssetIndex::GetStatus() const
{
    if (!bConfigured) { return TEXT("尚未配置检索范围"); }
    if (Registry->IsLoadingAssets()) { return TEXT("正在发现资产，当前结果不完整…"); }
    if (bRebuild || Build.IsValid()) { return TEXT("正在建立动画元数据索引…"); }
    if (!IsReady()) { return TEXT("正在合并资产变化…"); }
    return FString::Printf(TEXT("指定范围索引就绪：%d 个 AnimSequence"), Records.Num());
}

TSharedRef<const FSearchSnapshot, ESPMode::ThreadSafe> FAssetIndex::Snapshot() const
{
    if (!CachedSnapshot)
    {
        TSharedRef<FSearchSnapshot, ESPMode::ThreadSafe> Result = MakeShared<FSearchSnapshot, ESPMode::ThreadSafe>();
        Records.GenerateValueArray(Result->Records);
        Result->BySource = BySource;
        for (const FRecordPtr& Record : Result->Records)
        {
            if (!Record->Sources.ContainsByPredicate([](const FSourceRecord& Source) { return !Source.Candidates.IsEmpty(); }))
            {
                Result->UnknownRecords.Add(Record);
            }
        }
        CachedSnapshot = Result;
    }
    return CachedSnapshot.ToSharedRef();
}
}







