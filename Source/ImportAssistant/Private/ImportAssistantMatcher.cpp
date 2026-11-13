#include "ImportAssistantMatcher.h"
#include "ImportAssistantStorageTemplates.h"
#include "ImportAssistantAssetAccess.h"

#include "EditorFramework/AssetImportData.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace ImportAssistant
{
FString NormalizePath(FString Path)
{
    Path = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Path);
    FPaths::CollapseRelativeDirectories(Path);
    if (Path.Len() > 3) { Path.RemoveFromEnd(TEXT("/")); }
    return Path.ToLower();
}

FString SourceFingerprint(const FAssetData& Data)
{
    FString Json;
    if (!Data.GetTagValue(UObject::SourceFileTagName(), Json)) { Data.GetTagValue(FName(TEXT("SourceFile")), Json); }
    return Json;
}

FRecordPtr MakeRecord(const FAssetData& Data, const FString& DataSourceDirectory)
{
    TSharedRef<FAssetRecord, ESPMode::ThreadSafe> Record = MakeShared<FAssetRecord, ESPMode::ThreadSafe>();
    Record->ObjectPath = Data.GetSoftObjectPath();
    Record->Name = Data.AssetName.ToString();
    Record->PackageName = Data.PackageName.ToString();
    Record->AssetClassPath = Data.AssetClassPath;
    Record->SourceJson = SourceFingerprint(Data);
    Data.GetTagValue(FName(TEXT("Skeleton")), Record->Skeleton);
    FString Json;
    TOptional<FAssetImportInfo> Info;
    if (Data.GetTagValue(UObject::SourceFileTagName(), Json)) { Info = FAssetImportInfo::FromJson(MoveTemp(Json)); }
    else if (!Record->SourceJson.IsEmpty())
    {
        Info.Emplace();
        Info->Insert(Record->SourceJson);
    }
    // 原生 FromJson 忽略空来源，会把骨骼模型的 0/1/2 内容槽位压缩；还原空项，禁止把蒙皮误作几何更新。
    if (Info.IsSet())
    {
        TArray<TSharedPtr<FJsonValue>> RawSources;
        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Record->SourceJson), RawSources) && RawSources.Num() > Info->SourceFiles.Num())
        {
            TArray<FAssetImportInfo::FSourceFile> Original = MoveTemp(Info->SourceFiles);
            int32 Cursor = 0;
            for (const auto& Value : RawSources)
            {
                const auto JsonSource = Value->AsObject();
                FString Filename;
                if (JsonSource.IsValid()) { JsonSource->TryGetStringField(TEXT("RelativeFilename"), Filename); }
                if (Filename.IsEmpty()) { Info->SourceFiles.AddDefaulted(); }
                else if (Original.IsValidIndex(Cursor)) { Info->SourceFiles.Add(MoveTemp(Original[Cursor++])); }
            }
        }
    }
    FString PackageFilename;
    FPackageName::TryConvertLongPackageNameToFilename(Record->PackageName, PackageFilename);
    if (Info.IsSet())
    {
        for (const FAssetImportInfo::FSourceFile& File : Info->SourceFiles)
        {
            FSourceRecord& Source = Record->Sources.AddDefaulted_GetRef();
            Source.Original = File.RelativeFilename;
            Source.DisplayLabel = File.DisplayLabelName;
            // 原生 JSON 缺少时间时会读成 Unix 0；导入文件的零值及缺省值均显示未知，不伪造 1970 年版本。
            if (File.Timestamp > FDateTime::FromUnixTimestamp(0)) { Source.ImportedTimestampUtc = File.Timestamp; }
            if (Source.Original.IsEmpty()) { continue; }
            if (!FPaths::IsRelative(Source.Original)) { Source.Candidates.Add(NormalizePath(Source.Original)); }
            else
            {
                // 仅生成候选路径，不在建索引时访问每个源文件；命中时才验证解析优先级。
                if (!PackageFilename.IsEmpty()) { Source.Candidates.AddUnique(NormalizePath(FPaths::GetPath(PackageFilename) / Source.Original)); }
                if (!DataSourceDirectory.IsEmpty()) { Source.Candidates.AddUnique(NormalizePath(DataSourceDirectory / Source.Original)); }
                Source.Candidates.AddUnique(NormalizePath(Source.Original));
            }
        }
    }
    return Record;
}

FSourceVersionInfo DescribeSourceVersion(const FAssetRecord& Asset, int32 SourceIndex)
{
    FSourceVersionInfo Info;
    const int32 DisplayIndex = Asset.Sources.IsValidIndex(SourceIndex) ? SourceIndex : (Asset.Sources.Num() == 1 ? 0 : INDEX_NONE);
    auto Format = [](FDateTime Time)
    {
        return Time > FDateTime::FromUnixTimestamp(0) ? Time.ToString(TEXT("%Y-%m-%d %H:%M:%S")) + TEXT(" UTC") : FString(TEXT("未知"));
    };
    if (DisplayIndex != INDEX_NONE) { Info.Label = Format(Asset.Sources[DisplayIndex].ImportedTimestampUtc); }
    else if (Asset.Sources.Num() > 1) { Info.Label = TEXT("多来源（未确定槽位）"); }
    Info.Tooltip = TEXT("资产：") + Asset.ObjectPath.ToString();
    for (int32 Index = 0; Index < Asset.Sources.Num(); ++Index)
    {
        const FSourceRecord& Source = Asset.Sources[Index];
        Info.Tooltip += FString::Printf(TEXT("\n来源槽位 %d%s：%s\n版本时间：%s"), Index, Index == DisplayIndex ? TEXT("（本行显示）") : TEXT(""),
            *(Source.Original + (Source.DisplayLabel.IsEmpty() ? FString() : TEXT("（") + Source.DisplayLabel + TEXT("）"))), *Format(Source.ImportedTimestampUtc));
    }
    if (Asset.Sources.IsEmpty()) { Info.Tooltip += TEXT("\n没有可用的来源时间记录。"); }
    return Info;
}

TArray<FString> MapPath(const FString& Path, const TArray<FPathMapping>& Mappings, TArray<int32>* OutAppliedRules)
{
    TArray<FString> Results;
    if (OutAppliedRules) { OutAppliedRules->Reset(); }
    int32 BestLength = -1;
    for (int32 RuleIndex = 0; RuleIndex < Mappings.Num(); ++RuleIndex)
    {
        const FPathMapping& Mapping = Mappings[RuleIndex];
        const FString Prefix = Mapping.From.EndsWith(TEXT("/")) ? Mapping.From : Mapping.From + TEXT("/");
        if (Path == Mapping.From || Path.StartsWith(Prefix))
        {
            if (Mapping.From.Len() > BestLength)
            {
                Results.Reset();
                if (OutAppliedRules) { OutAppliedRules->Reset(); }
                BestLength = Mapping.From.Len();
            }
            if (Mapping.From.Len() == BestLength)
            {
                Results.AddUnique(NormalizePath(Mapping.To / Path.RightChop(Prefix.Len())));
                if (OutAppliedRules) { OutAppliedRules->Add(RuleIndex); }
            }
        }
    }
    return Results;
}

namespace
{
TArray<FString> ParseKeywords(const FString& File, const FMatchSettings& Settings)
{
    FString Stem = FPaths::GetBaseFilename(File);
    for (FString Prefix : Settings.IgnorePrefixes)
    {
        Prefix.TrimStartAndEndInline();
        if (!Prefix.IsEmpty() && Stem.RemoveFromStart(Prefix, ESearchCase::IgnoreCase)) { break; }
    }
    for (FString Suffix : Settings.IgnoreSuffixes)
    {
        Suffix.TrimStartAndEndInline();
        if (!Suffix.IsEmpty() && Stem.RemoveFromEnd(Suffix, ESearchCase::IgnoreCase)) { break; }
    }
    const TCHAR* Delimiters[] = { TEXT("_"), TEXT("-"), TEXT(" "), TEXT(".") };
    TArray<FString> Tokens;
    Stem.ParseIntoArray(Tokens, Delimiters, UE_ARRAY_COUNT(Delimiters), true);
    return Tokens;
}

bool MatchKeywordTokens(const FString& AssetName, const TArray<FString>& Tokens, bool bAllKeywords, TArray<FString>* OutMatchedKeywords = nullptr)
{
    if (OutMatchedKeywords) { OutMatchedKeywords->Reset(); }
    bool bAnyFound = false;
    for (const FString& Token : Tokens)
    {
        const bool bFound = AssetName.Contains(Token, ESearchCase::IgnoreCase);
        if (bAllKeywords && !bFound)
        {
            if (OutMatchedKeywords) { OutMatchedKeywords->Reset(); }
            return false;
        }
        if (bFound)
        {
            bAnyFound = true;
            if (OutMatchedKeywords) { OutMatchedKeywords->AddUnique(Token); }
            else if (!bAllKeywords) { return true; }
        }
    }
    return bAnyFound;
}
}

bool MatchesKeywords(const FString& AssetName, const FString& File, const FMatchSettings& Settings, TArray<FString>* OutMatchedKeywords)
{
    return MatchKeywordTokens(AssetName, ParseKeywords(File, Settings), Settings.bAllKeywords, OutMatchedKeywords);
}
FString DescribeReason(EMatchReason Reason)
{
    TArray<FString> Labels;
    if (EnumHasAnyFlags(Reason, EMatchReason::Exact)) { Labels.Add(TEXT("源路径")); }
    if (EnumHasAnyFlags(Reason, EMatchReason::Mapping)) { Labels.Add(TEXT("路径映射")); }
    if (EnumHasAnyFlags(Reason, EMatchReason::Keyword)) { Labels.Add(TEXT("关键词候选")); }
    return Labels.IsEmpty() ? TEXT("无匹配依据") : FString::Join(Labels, TEXT(" + "));
}

FSearchOutput Match(const FSearchSnapshot& Index, const TArray<FString>& Files, const FMatchSettings& Settings,
    const TArray<FPathMapping>& Mappings, const std::atomic_bool& Cancelled, const TArray<FFileSearchScope>& Scopes)
{
    const double Start = FPlatformTime::Seconds();
    FSearchOutput Output;
    if (Files.IsEmpty()) { return Output; }
    TMap<FSoftObjectPath, TArray<FRowPtr>> MatchesByAsset;
    TMultiMap<FString, const FFileSearchScope*> ScopeByFile;
    for (const FFileSearchScope& Scope : Scopes) { ScopeByFile.Add(NormalizePath(Scope.File), &Scope); }
    TMap<FString, bool> ExistsCache;
    auto Exists = [&ExistsCache](const FString& Path)
    {
        if (const bool* Found = ExistsCache.Find(Path)) { return *Found; }
        const bool bExists = IFileManager::Get().FileExists(*Path);
        ExistsCache.Add(Path, bExists);
        return bExists;
    };
    for (const FString& File : Files)
    {
        if (Cancelled.load()) { return {}; }
        const int32 RowsBeforeFile = Output.Rows.Num();
        const FString FileKey = NormalizePath(File);
        TArray<const FFileSearchScope*> FileScopes;
        ScopeByFile.MultiFind(FileKey, FileScopes);
        auto FindScope = [&FileScopes](const FRecordPtr& Record) -> const FFileSearchScope*
        {
            for (const auto* Scope : FileScopes)
            {
                if (Scope->AssetClassPath.IsNull() || Scope->AssetClassPath == Record->AssetClassPath) { return Scope; }
            }
            return nullptr;
        };
        auto InScope = [&FindScope, &Scopes](const FRecordPtr& Record)
        {
            const auto* Scope = FindScope(Record);
            return Scopes.IsEmpty() || (Scope && IsDirectoryInRoots(FPackageName::GetLongPackagePath(Record->PackageName), Scope->SearchRoots, Scope->bRecursive));
        };
        TArray<FString> Keywords;
        if (Settings.bMatchKeyword) { Keywords = ParseKeywords(File, Settings); }
        TSet<FRecordPtr> Candidates;
        if (Settings.bMatchPath)
        {
            if (const TArray<FRecordPtr>* Direct = Index.BySource.Find(FileKey)) { Candidates.Append(*Direct); }
            // 映射启用时只遍历来源键，不加载资产；默认无映射的精确查询仍是哈希查询。
            if (!Mappings.IsEmpty())
            {
                for (const auto& Pair : Index.BySource)
                {
                    if (Cancelled.load()) { return {}; }
                    if (MapPath(Pair.Key, Mappings).Contains(FileKey)) { Candidates.Append(Pair.Value); }
                }
            }
        }
        if (Settings.bMatchKeyword)
        {
            for (const FRecordPtr& Record : Index.Records)
            {
                if (Cancelled.load()) { return {}; }
                if (InScope(Record) && MatchKeywordTokens(Record->Name, Keywords, Settings.bAllKeywords)) { Candidates.Add(Record); }
            }
        }
        for (const FRecordPtr& Record : Candidates)
        {
            if (Cancelled.load()) { return {}; }
            if (!InScope(Record)) { continue; }
            FRowPtr Row = MakeShared<FMatchRow>();
            Row->Asset = Record;
            Row->File = File;
            TSet<int32> MatchedSlots;
            TArray<FString> Evidence;
            if (Settings.bMatchPath)
            {
                for (int32 SourceIndex = 0; SourceIndex < Record->Sources.Num(); ++SourceIndex)
                {
                    const FSourceRecord& Source = Record->Sources[SourceIndex];
                    FString Resolved;
                    for (const FString& Candidate : Source.Candidates)
                    {
                        Resolved = Candidate;
                        if (Exists(Candidate)) { break; }
                    }
                    if (Resolved == FileKey)
                    {
                        Row->Reason |= EMatchReason::Exact;
                        MatchedSlots.Add(SourceIndex);
                        Evidence.Add(FString::Printf(TEXT("源路径匹配（来源项 %d）\n资产记录：%s\n解析路径：%s\n本次 FBX：%s"),
                            SourceIndex + 1, *Source.Original, *Resolved, *File));
                    }
                    for (const FString& Candidate : Source.Candidates)
                    {
                        TArray<int32> AppliedRules;
                        const TArray<FString> Mapped = MapPath(Candidate, Mappings, &AppliedRules);
                        if (Mapped.Contains(FileKey))
                        {
                            Row->Reason |= EMatchReason::Mapping;
                            MatchedSlots.Add(SourceIndex);
                            Row->bSourceAmbiguous |= Mapped.Num() > 1;
                            for (const int32 RuleIndex : AppliedRules)
                            {
                                const FPathMapping& Rule = Mappings[RuleIndex];
                                const FString Prefix = Rule.From.EndsWith(TEXT("/")) ? Rule.From : Rule.From + TEXT("/");
                                const FString Destination = NormalizePath(Rule.To / Candidate.RightChop(Prefix.Len()));
                                if (Destination != FileKey) { continue; }
                                Evidence.AddUnique(FString::Printf(TEXT("路径映射匹配（来源项 %d）\n资产记录：%s\n映射规则：%s => %s\n"
                                    "映射前：%s\n映射后：%s\n本次 FBX：%s%s"), SourceIndex + 1, *Source.Original, *Rule.From, *Rule.To,
                                    *Candidate, *Destination, *File, Mapped.Num() > 1 ? TEXT("\n存在同优先级映射歧义，禁止直接更新") : TEXT("")));
                            }
                        }
                    }
                }
            }
            TArray<FString> MatchedKeywords;
            if (Settings.bMatchKeyword && MatchKeywordTokens(Record->Name, Keywords, Settings.bAllKeywords, &MatchedKeywords))
            {
                Row->Reason |= EMatchReason::Keyword;
                Evidence.Add(FString::Printf(TEXT("名称关键词匹配：%s\n命中关键词：%s\n匹配对象：%s"),
                    Settings.bAllKeywords ? TEXT("全部关键词") : TEXT("任意关键词"), *FString::Join(MatchedKeywords, TEXT("、")), *Record->Name));
            }
            if (Row->Reason == EMatchReason::None) { continue; }
            if (MatchedSlots.Num() == 1) { Row->SourceIndex = *MatchedSlots.CreateConstIterator(); }
            else if (MatchedSlots.Num() > 1) { Row->bSourceAmbiguous = true; }
            else if (Record->Sources.Num() == 1 && !Record->Sources[0].Original.IsEmpty()) { Row->SourceIndex = 0; }
            Row->Status = Row->CanSelect() ? TEXT("未选择") : TEXT("来源槽位缺失或歧义，需先修复资产来源");
            const auto* Scope = FindScope(Record);
            if (Scope && !Scope->TemplateRoots.IsEmpty())
            {
                Row->bOutsideTemplate = !IsDirectoryInRoots(FPackageName::GetLongPackagePath(Record->PackageName), Scope->TemplateRoots, Scope->bRecursive);
                Evidence.Add((Row->bOutsideTemplate ? TEXT("模板目录外候选：来自原资产目录补查，不会因不符合存放规范而遗漏。\n") :
                    TEXT("资产位于本次推导的模板目录内。\n")) + Scope->Details);
            }
            if (Record->AssetClassPath == GetAssetClassPath(EImportAssistantAssetCategory::SkeletalMesh))
            {
                Evidence.Add(DescribeSourceSlot(Record->AssetClassPath, Row->SourceIndex));
            }
            // 悬停直接使用本次匹配证据，不再依赖后来修改的设置或重新查询资产。
            Row->ReasonTooltip = FString::Join(Evidence, TEXT("\n\n"));
            MatchesByAsset.FindOrAdd(Record->ObjectPath).Add(Row);
            Output.Rows.Add(Row);
        }
        if (Output.Rows.Num() == RowsBeforeFile) { Output.UnmatchedFiles.Add(File); }
    }
    for (auto& Pair : MatchesByAsset)
    {
        if (Pair.Value.Num() > 1)
        {
            for (const FRowPtr& Row : Pair.Value)
            {
                Row->bFileConflict = true;
                if (Row->CanSelect()) { Row->Status = TEXT("多个 FBX 命中：勾选一个来源，其余自动取消"); }
            }
        }
    }

    for (const FRecordPtr& Record : Index.UnknownRecords)
    {
        if (Cancelled.load()) { return {}; }
        const FString Directory = FPackageName::GetLongPackagePath(Record->PackageName);
        if (!Scopes.IsEmpty() && !Scopes.ContainsByPredicate([&Directory, &Record](const FFileSearchScope& Scope)
            { return (Scope.AssetClassPath.IsNull() || Scope.AssetClassPath == Record->AssetClassPath) &&
                IsDirectoryInRoots(Directory, Scope.SearchRoots, Scope.bRecursive); })) { continue; }
        // 范围诊断与匹配结果分离；已由关键词命中的未知来源候选保留在对应 FBX 组，不重复计入“另有”数量。
        if (!MatchesByAsset.Contains(Record->ObjectPath)) { ++Output.UnmatchedUnknownCount; }
    }
    Output.Rows.Sort([](const FRowPtr& A, const FRowPtr& B)
    {
        if (A->File == B->File && A->bOutsideTemplate != B->bOutsideTemplate) { return !A->bOutsideTemplate; }
        return A->File == B->File ? A->Asset->ObjectPath.ToString() < B->Asset->ObjectPath.ToString() : A->File < B->File;
    });
    Output.Seconds = FPlatformTime::Seconds() - Start;
    return Output;
}
}








