#pragma once

#include "ImportAssistantTypes.h"

namespace ImportAssistant
{
struct FResultNode;
using FResultNodePtr = TSharedPtr<FResultNode>;

struct FResultNode
{
    FString File;
    FString Title;
    FString Tooltip;
    FString ScopeSummary;
    FString ScopeDetails;
    FAutoReimportInfo AutoReimport;
    FRowPtr Row;
    TArray<FResultNodePtr> Children;
    int32 SelectableCount = 0;
    int32 SelectedCount = 0;
    bool bGroup = false;
    bool bLastInGroup = false;
};

TArray<FResultNodePtr> BuildResultGroups(const TArray<FString>& Files, const TArray<FRowPtr>& Rows, bool bPending, const TArray<FFileSearchScope>& Scopes = {},
    const TMap<FString, FAutoReimportInfo>& AutoReimportFiles = {});
}



