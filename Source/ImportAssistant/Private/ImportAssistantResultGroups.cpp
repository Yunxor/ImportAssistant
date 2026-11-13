#include "ImportAssistantResultGroups.h"

#include "ImportAssistantMatcher.h"
#include "ImportAssistantAutoReimport.h"
#include "Misc/Paths.h"

namespace ImportAssistant
{
TArray<FResultNodePtr> BuildResultGroups(const TArray<FString>& Files, const TArray<FRowPtr>& Rows, bool bPending, const TArray<FFileSearchScope>& Scopes, const TMap<FString, FAutoReimportInfo>& AutoReimportFiles)
{
    TMultiMap<FString, const FFileSearchScope*> ScopeByFile;
    for (const FFileSearchScope& Scope : Scopes) { ScopeByFile.Add(NormalizePath(Scope.File), &Scope); }
    TArray<FResultNodePtr> Groups;
    TMap<FString, FResultNodePtr> ByFile;
    for (const FString& File : Files)
    {
        const FString Key = NormalizePath(File);
        if (ByFile.Contains(Key)) { continue; }
        FResultNodePtr Group = MakeShared<FResultNode>();
        Group->bGroup = true;
        Group->File = File;
        Group->Title = FPaths::GetBaseFilename(File);
        Group->Tooltip = File;
        TArray<const FFileSearchScope*> FileScopes;
        ScopeByFile.MultiFind(Key, FileScopes, true);
        for (const auto* Scope : FileScopes)
        {
            if (!Group->ScopeSummary.IsEmpty()) { Group->ScopeSummary += TEXT("\n"); Group->ScopeDetails += TEXT("\n\n"); }
            Group->ScopeSummary += Scope->Summary;
            if (!Scope->TemplateRoots.IsEmpty()) { Group->ScopeSummary += TEXT("\n目标目录：") + FString::Join(Scope->TemplateRoots, TEXT("；")); }
            Group->ScopeDetails += Scope->Details;
        }
        Groups.Add(Group);
        ByFile.Add(Key, Group);
    }
    for (const FRowPtr& Row : Rows)
    {
        const FResultNodePtr* Found = ByFile.Find(NormalizePath(Row->File));
        if (!Found) { continue; }
        const FResultNodePtr& Group = *Found;
        FResultNodePtr Child = MakeShared<FResultNode>();
        Child->Row = Row;
        Group->Children.Add(Child);
        if (Row->CanSelect())
        {
            ++Group->SelectableCount;
            Group->SelectedCount += Row->bSelected ? 1 : 0;
        }
    }
    for (const FResultNodePtr& Group : Groups)
    {
        if (!Group->File.IsEmpty())
        {
            if (const FAutoReimportInfo* FileInfo = AutoReimportFiles.Find(NormalizePath(Group->File)))
            {
                TArray<FRowPtr> Assets;
                for (const FResultNodePtr& Child : Group->Children) { if (Child->Row) { Assets.Add(Child->Row); } }
                Group->AutoReimport = SummarizeAutoReimport(*FileInfo, Assets, bPending);
            }
        }
        Group->Children.Sort([](const FResultNodePtr& A, const FResultNodePtr& B)
        {
            if (A->Row->bOutsideTemplate != B->Row->bOutsideTemplate) { return !A->Row->bOutsideTemplate; }
            return A->Row->Asset->ObjectPath.ToString() < B->Row->Asset->ObjectPath.ToString();
        });
        if (Group->Children.IsEmpty())
        {
            FResultNodePtr Empty = MakeShared<FResultNode>();
            Empty->Title = bPending ? TEXT("等待检索完成…") : TEXT("指定范围内未找到匹配资产");
            Empty->Tooltip = Group->File;
            Group->Children.Add(Empty);
        }
        Group->Children.Last()->bLastInGroup = true;
    }
    return Groups;
}
}



