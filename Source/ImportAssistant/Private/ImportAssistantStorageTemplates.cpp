#include "ImportAssistantStorageTemplates.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace ImportAssistant
{
bool IsDirectoryInRoots(const FString& Directory, const TArray<FString>& Roots, bool bRecursive)
{
    for (const FString& Root : Roots)
    {
        if (Directory.Equals(Root, ESearchCase::IgnoreCase) || (bRecursive && Directory.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase))) { return true; }
    }
    return false;
}

void CompactStorageRoots(TArray<FString>& Roots, bool bRecursive)
{
    Roots.Sort([](const FString& A, const FString& B)
    {
        return A.Len() == B.Len() ? A.Compare(B, ESearchCase::IgnoreCase) < 0 : A.Len() < B.Len();
    });
    TArray<FString> Compact;
    for (const FString& Root : Roots)
    {
        if (!IsDirectoryInRoots(Root, Compact, bRecursive)) { Compact.Add(Root); }
    }
    Compact.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
    Roots = MoveTemp(Compact);
}

namespace
{
struct FCompiledTemplate
{
    const FImportAssistantStorageTemplate* Definition;
    FString Pattern;
    TArray<FString> References;
};

bool IsValidDirectory(const FString& Path, FString& OutReason)
{
    if (!Path.StartsWith(TEXT("/")) || Path.Contains(TEXT("\\")) || Path.Contains(TEXT("//")) || Path.Contains(TEXT("{")) || Path.Contains(TEXT("}")))
    {
        OutReason = TEXT("必须使用以 / 开头的 UE 目录，不能包含反斜线、空路径段或未替换的变量");
        return false;
    }
    TArray<FString> Parts;
    Path.ParseIntoArray(Parts, TEXT("/"), true);
    for (const FString& Part : Parts)
    {
        if (Part == TEXT(".") || Part == TEXT("..")) { OutReason = TEXT("目录不能包含 . 或 .. 越级路径"); return false; }
    }
    FString DiskPath;
    if (!FPackageName::IsValidLongPackageName(Path / TEXT("Probe")) || !FPackageName::TryConvertLongPackageNameToFilename(Path / TEXT("Probe"), DiskPath))
    {
        OutReason = TEXT("目录包含非法字符，或内容根目录尚未挂载");
        return false;
    }
    return true;
}

bool CompileTemplate(const FImportAssistantStorageTemplate& Template, FCompiledTemplate& Out, FString& OutError)
{
    Out.Definition = &Template;
    Out.Pattern = Template.DirectoryTemplate.TrimStartAndEnd();
    Out.Pattern.RemoveFromEnd(TEXT("/"));
    if (Template.TemplateName.TrimStartAndEnd().IsEmpty() || Template.Delimiter.IsEmpty())
    {
        OutError = TEXT("模板名称和分隔符不能为空");
        return false;
    }
    TArray<FString> Variables;
    for (const FImportAssistantNameField& Field : Template.Fields)
    {
        if (Field.Position < 1 || Field.Variable.IsEmpty() || (!FChar::IsAlpha(Field.Variable[0]) && Field.Variable[0] != TCHAR('_')))
        {
            OutError = TEXT("词段位置必须从 1 开始，变量名必须以字母或下划线开头");
            return false;
        }
        for (const TCHAR Character : Field.Variable)
        {
            if (!FChar::IsAlnum(Character) && Character != TCHAR('_')) { OutError = TEXT("变量名仅允许字母、数字、下划线"); return false; }
        }
        if (Variables.ContainsByPredicate([&Field](const FString& Name) { return Name.Equals(Field.Variable, ESearchCase::CaseSensitive); }))
        {
            OutError = TEXT("变量名重复：") + Field.Variable;
            return false;
        }
        Variables.Add(Field.Variable);
    }
    FString Sample;
    for (int32 Cursor = 0; Cursor < Out.Pattern.Len();)
    {
        if (Out.Pattern[Cursor] == TCHAR('}')) { OutError = TEXT("目录模板含有未配对的 }"); return false; }
        if (Out.Pattern[Cursor] != TCHAR('{')) { Sample.AppendChar(Out.Pattern[Cursor++]); continue; }
        const int32 End = Out.Pattern.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor + 1);
        if (End == INDEX_NONE) { OutError = TEXT("目录模板中的 { 没有结束符 }"); return false; }
        const FString Variable = Out.Pattern.Mid(Cursor + 1, End - Cursor - 1);
        if (!Variables.ContainsByPredicate([&Variable](const FString& Name) { return Name.Equals(Variable, ESearchCase::CaseSensitive); }))
        {
            OutError = TEXT("目录引用了未定义变量：") + Variable;
            return false;
        }
        Out.References.Add(Variable);
        Sample += TEXT("TemplateValue");
        Cursor = End + 1;
    }
    const int32 RootEnd = Out.Pattern.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
    const FString RootPart = RootEnd == INDEX_NONE ? Out.Pattern : Out.Pattern.Left(RootEnd);
    if (RootPart.Contains(TEXT("{"))) { OutError = TEXT("内容根目录不能使用变量，请写 /Game 或已挂载插件根"); return false; }
    if (!IsValidDirectory(Sample, OutError)) { return false; }
    return true;
}
}

bool BuildStorageSearchPlan(const TArray<FImportAssistantStorageTemplate>& Templates, bool bEnabled,
    EImportAssistantStorageSearchMode Mode, EImportAssistantAssetCategory Category, const TArray<FString>& Files,
    const TArray<FName>& BaseRoots, bool bRecursive, TArray<FFileSearchScope>& OutScopes, TArray<FName>& OutIndexRoots, FString& OutError)
{
    OutScopes.Reset();
    OutIndexRoots.Reset();
    OutError.Reset();
    if (!bEnabled) { OutIndexRoots = BaseRoots; return true; }
    TArray<FCompiledTemplate> Compiled;
    for (int32 Index = 0; Index < Templates.Num(); ++Index)
    {
        const FImportAssistantStorageTemplate& Template = Templates[Index];
        if (!Template.bEnabled) { continue; }
        FCompiledTemplate Value;
        if (!CompileTemplate(Template, Value, OutError))
        {
            OutError = FString::Printf(TEXT("存放模板 %d「%s」配置错误：%s"), Index + 1, *Template.TemplateName, *OutError);
            return false;
        }
        if (Template.AssetCategory == Category) { Compiled.Add(MoveTemp(Value)); }
    }
    TArray<FString> FallbackRoots;
    for (const FName Root : BaseRoots) { FallbackRoots.Add(Root.ToString()); }
    CompactStorageRoots(FallbackRoots, bRecursive);
    TArray<FString> IndexRoots;
    for (const FString& File : Files)
    {
        FFileSearchScope Scope;
        Scope.File = File;
        Scope.bRecursive = bRecursive;
        TArray<FString> Preview;
        TArray<FString> Names;
        TArray<FString> ParsedFields;
        for (const FCompiledTemplate& Value : Compiled)
        {
            const FImportAssistantStorageTemplate& Template = *Value.Definition;
            FString Stem = FPaths::GetBaseFilename(File);
            if (!Template.NamePrefix.IsEmpty() && !Stem.RemoveFromStart(Template.NamePrefix, ESearchCase::IgnoreCase))
            {
                Preview.Add(FString::Printf(TEXT("模板「%s」未适用：名称缺少前缀 %s"), *Template.TemplateName, *Template.NamePrefix));
                continue;
            }
            TArray<FString> Tokens;
            Stem.ParseIntoArray(Tokens, *Template.Delimiter, false);
            TArray<TPair<FString, FString>> Values;
            TArray<FString> Fields;
            FString Failure;
            for (const FImportAssistantNameField& Field : Template.Fields)
            {
                const FString Token = Tokens.IsValidIndex(Field.Position - 1) ? Tokens[Field.Position - 1] : FString();
                Fields.Add(FString::Printf(TEXT("%s = %s（第 %d 段%s%s）"), *Field.Variable, Token.IsEmpty() ? TEXT("<缺失或为空>") : *Token,
                    Field.Position, Field.Description.IsEmpty() ? TEXT("") : TEXT("，"), *Field.Description));
                if (!Value.References.ContainsByPredicate([&Field](const FString& Name)
                    { return Name.Equals(Field.Variable, ESearchCase::CaseSensitive); })) { continue; }
                if (Token.IsEmpty()) { Failure = TEXT("目录所需字段缺失或为空：") + Field.Variable; break; }
                FString Reason;
                if (Token.Contains(TEXT("/")) || Token.Contains(TEXT("\\")) || !IsValidDirectory(TEXT("/Game/") + Token, Reason))
                {
                    Failure = TEXT("字段不能作为单个合法目录名：") + Field.Variable + TEXT(" = ") + Token;
                    break;
                }
                Values.Emplace(Field.Variable, Token);
            }
            FString Directory = Value.Pattern;
            if (Failure.IsEmpty())
            {
                for (const auto& Pair : Values) { Directory.ReplaceInline(*(TEXT("{") + Pair.Key + TEXT("}")), *Pair.Value, ESearchCase::CaseSensitive); }
                IsValidDirectory(Directory, Failure);
            }
            if (!Failure.IsEmpty())
            {
                Preview.Add(FString::Printf(TEXT("模板「%s」无法解析：%s\n%s"), *Template.TemplateName, *Failure, *FString::Join(Fields, TEXT("；"))));
                continue;
            }
            Scope.TemplateRoots.Add(Directory);
            Names.Add(Template.TemplateName);
            ParsedFields.Add(Template.TemplateName + TEXT("：") + FString::Join(Fields, TEXT("；")));
            Preview.Add(FString::Printf(TEXT("模板：%s\n规范：%s\n解析：%s\n目录模板：%s\n目标目录：%s"), *Template.TemplateName,
                *Template.Description, *FString::Join(Fields, TEXT("；")), *Template.DirectoryTemplate, *Directory));
        }
        CompactStorageRoots(Scope.TemplateRoots, bRecursive);
        Scope.bLimitedToTemplates = Mode == EImportAssistantStorageSearchMode::TemplateOnly && !Scope.TemplateRoots.IsEmpty();
        Scope.SearchRoots = Scope.TemplateRoots;
        if (!Scope.bLimitedToTemplates) { Scope.SearchRoots.Append(FallbackRoots); }
        CompactStorageRoots(Scope.SearchRoots, bRecursive);
        const FString Strategy = Scope.TemplateRoots.IsEmpty() ? TEXT("没有可用模板，回退原资产目录") :
            (Scope.bLimitedToTemplates ? TEXT("仅模板目录；未检查模板目录外资产") : TEXT("模板目录优先，并补查原资产目录"));
        if (Scope.TemplateRoots.IsEmpty())
        {
            Scope.Summary = Strategy;
            if (Compiled.IsEmpty()) { Preview.Add(TEXT("没有启用的当前资产类别模板；动画窗口不会使用模型模板")); }
        }
        else
        {
            Scope.Summary = TEXT("模板：") + FString::Join(Names, TEXT("、")) + TEXT("；") + Strategy +
                TEXT("\n解析：") + FString::Join(ParsedFields, TEXT(" | "));
        }
        Preview.Add(TEXT("检索策略：") + Strategy + TEXT("\n实际范围：") + FString::Join(Scope.SearchRoots, TEXT("；")) +
            (bRecursive ? TEXT("（递归子目录）") : TEXT("（仅目录本层）")));
        Scope.Details = FString::Join(Preview, TEXT("\n\n"));
        IndexRoots.Append(Scope.SearchRoots);
        OutScopes.Add(MoveTemp(Scope));
    }
    if (Files.IsEmpty()) { IndexRoots = FallbackRoots; }
    CompactStorageRoots(IndexRoots, bRecursive);
    for (const FString& Root : IndexRoots) { OutIndexRoots.Add(FName(Root)); }
    return true;
}
}


