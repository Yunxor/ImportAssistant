#pragma once

#include "ImportAssistantTypes.h"

namespace ImportAssistant
{
struct FAssetTypePresentation
{
    FText DisplayName;
    FString ClassName;
    FLinearColor Color = FLinearColor::Gray;
    FText Label;
};

class FAssetPresentationCache
{
public:
    const FAssetTypePresentation& Get(const FTopLevelAssetPath& ClassPath);

private:
    TMap<FTopLevelAssetPath, FAssetTypePresentation> Types;
    uint64 DefinitionVersion = MAX_uint64;
};

FText DescribeAssetFileTypes(const FAssetRecord& Asset, const FAssetTypePresentation& Presentation);
}
