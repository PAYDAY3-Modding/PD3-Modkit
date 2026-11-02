#include "ModBrowser/CrimeFactoryModOrganisation.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "CrimeFactoryModOrganisation"

const FString FCrimeFactoryModOrganisation::AllMods(TEXT("__all_mods__"));

FCrimeFactoryModOrganisation::FCrimeFactoryModOrganisation(const FString& InFilename)
    : Filename(InFilename.IsEmpty() ? FPaths::GeneratedConfigDir() / TEXT("CrimeFactoryOrganisation.json") : InFilename)
{
}

FString FCrimeFactoryModOrganisation::ModKey(const IPlugin& Plugin)
{
    FString Path = FPaths::ConvertRelativePathToFull(Plugin.GetDescriptorFileName());
    FPaths::NormalizeFilename(Path);
    FPaths::CollapseRelativeDirectories(Path);
    // Project-relative identities survive moving the entire project. Display names
    // never identify mods. Directory renames require a separate fingerprint check.
    for (const auto& Root : { TPair<FString, FString>(TEXT("project:"), FPaths::ProjectDir()),
                              TPair<FString, FString>(TEXT("engine:"), FPaths::EngineDir()) })
    {
        FString Base = FPaths::ConvertRelativePathToFull(Root.Value);
        FPaths::NormalizeDirectoryName(Base);
        Base += TEXT("/");
        if (Path.StartsWith(Base, ESearchCase::IgnoreCase))
        {
            return (Root.Key + Path.Mid(Base.Len())).ToLower();
        }
    }
    return TEXT("external:") + Path.ToLower();
}

TArray<FCrimeFactoryModFolder> FCrimeFactoryModOrganisation::Children(const FString& Parent) const
{
    TArray<FCrimeFactoryModFolder> Result;
    for (const auto& Pair : Folders)
    {
        if (Pair.Value.Parent == Parent) Result.Add(Pair.Value);
    }
    Result.Sort([](const auto& A, const auto& B) { return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0; });
    return Result;
}

bool FCrimeFactoryModOrganisation::IsWithin(const FString& Folder, const FString& Ancestor) const
{
    FString Cursor = Folder;
    for (int32 Depth = 0; Depth <= Folders.Num(); ++Depth)
    {
        if (Cursor == Ancestor) return true;
        const FCrimeFactoryModFolder* Node = Find(Cursor);
        if (!Node) return false;
        Cursor = Node->Parent;
    }
    return false;
}

FString FCrimeFactoryModOrganisation::FolderForMod(const FString& Key) const
{
    const FString* Id = ModFolders.Find(Key);
    return Id && HasFolder(*Id) ? *Id : FString();
}

bool FCrimeFactoryModOrganisation::ValidateName(const FString& Parent, const FString& Name, const FString& ExceptId, FText& Error) const
{
    const FString Trimmed = Name.TrimStartAndEnd();
    int32 ParentDepth = 0;
    FString Ancestor = Parent;
    while (const auto* Node = Find(Ancestor))
    {
        if (++ParentDepth >= 128)
        {
            Error = LOCTEXT("FolderDepth", "Folder hierarchies can contain up to 128 levels.");
            return false;
        }
        Ancestor = Node->Parent;
    }
    bool bHasControl = false;
    for (TCHAR Character : Trimmed) bHasControl |= FChar::IsControl(Character);
    if (!HasFolder(Parent) || Trimmed.IsEmpty() || Trimmed.Len() > 128 || Trimmed == TEXT(".") || Trimmed == TEXT("..") ||
        Trimmed.Contains(TEXT("/")) || Trimmed.Contains(TEXT("\\")) ||
        bHasControl)
    {
        Error = LOCTEXT("InvalidFolderName", "Choose a folder name of 1 to 128 characters, without slashes or control characters.");
        return false;
    }
    for (const auto& Pair : Folders)
    {
        if (Pair.Key != ExceptId && Pair.Value.Parent == Parent && Pair.Value.Name.Equals(Trimmed, ESearchCase::IgnoreCase))
        {
            Error = LOCTEXT("DuplicateFolder", "A folder with that name already exists here.");
            return false;
        }
    }
    return true;
}

bool FCrimeFactoryModOrganisation::CreateFolder(const FString& Parent, const FString& Name, FString& NewId, FText& Error)
{
    if (!ValidateName(Parent, Name, FString(), Error)) return false;
    NewId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Folders.Add(NewId, { NewId, Parent, Name.TrimStartAndEnd() });
    return true;
}

bool FCrimeFactoryModOrganisation::RenameFolder(const FString& Id, const FString& Name, FText& Error)
{
    FCrimeFactoryModFolder* Node = Folders.Find(Id);
    if (!Node || !ValidateName(Node->Parent, Name, Id, Error)) return false;
    Node->Name = Name.TrimStartAndEnd();
    return true;
}

bool FCrimeFactoryModOrganisation::CanMoveFolder(const FString& Id, const FString& Parent, FText& Error) const
{
    const FCrimeFactoryModFolder* Node = Find(Id);
    if (!Node || !HasFolder(Parent) || IsWithin(Parent, Id))
    {
        Error = LOCTEXT("InvalidFolderMove", "A folder cannot be moved into itself or one of its descendants.");
        return false;
    }
    int32 ParentDepth = 0;
    FString Cursor = Parent;
    while (const auto* Ancestor = Find(Cursor)) { ++ParentDepth; Cursor = Ancestor->Parent; }
    for (const auto& Pair : Folders)
    {
        if (!IsWithin(Pair.Key, Id)) continue;
        int32 Depth = 1;
        Cursor = Pair.Key;
        while (Cursor != Id) { ++Depth; Cursor = Find(Cursor)->Parent; }
        if (ParentDepth + Depth > 128)
        {
            Error = LOCTEXT("MoveTooDeep", "This move would exceed the limit of 128 folder levels.");
            return false;
        }
    }
    return ValidateName(Parent, Node->Name, Id, Error);
}

bool FCrimeFactoryModOrganisation::MoveFolder(const FString& Id, const FString& Parent, FText& Error)
{
    if (!CanMoveFolder(Id, Parent, Error)) return false;
    Folders.FindChecked(Id).Parent = Parent;
    return true;
}

bool FCrimeFactoryModOrganisation::DeleteFolder(const FString& Id)
{
    const FCrimeFactoryModFolder* Node = Find(Id);
    if (!Node) return false;
    const FString Parent = Node->Parent;
    TSet<FString> Removed;
    for (const auto& Pair : Folders) if (IsWithin(Pair.Key, Id)) Removed.Add(Pair.Key);
    for (auto& Pair : ModFolders) if (Removed.Contains(Pair.Value)) Pair.Value = Parent;
    if (Removed.Contains(CurrentFolder)) CurrentFolder = Parent;
    for (const FString& RemovedId : Removed)
    {
        ExpandedFolders.Remove(RemovedId);
        Folders.Remove(RemovedId);
    }
    return true;
}

bool FCrimeFactoryModOrganisation::AssignMods(const TArray<FString>& Keys, const FString& Folder)
{
    if (!HasFolder(Folder)) return false;
    bool bChanged = false;
    for (const FString& Key : Keys)
    {
        if (!Key.IsEmpty() && FolderForMod(Key) != Folder)
        {
            if (Folder.IsEmpty()) ModFolders.Remove(Key);
            else ModFolders.Add(Key, Folder);
            if (Folder.IsEmpty()) ModFingerprints.Remove(Key);
            else if (const auto* Fingerprint = DiscoveredFingerprints.Find(Key)) ModFingerprints.Add(Key, *Fingerprint);
            bChanged = true;
        }
    }
    return bChanged;
}

bool FCrimeFactoryModOrganisation::ReconcileDescriptors(const TMap<FString, FString>& Descriptors)
{
    DiscoveredFingerprints.Reset();
    TMap<FString, int32> SignatureCounts;
    for (const auto& Pair : Descriptors)
    {
        TArray<uint8> Bytes;
        if (FFileHelper::LoadFileToArray(Bytes, *Pair.Value))
        {
            const FString Signature = FPaths::GetBaseFilename(Pair.Value).ToLower() + TEXT(":") + FMD5::HashBytes(Bytes.GetData(), Bytes.Num());
            DiscoveredFingerprints.Add(Pair.Key, Signature);
            ++SignatureCounts.FindOrAdd(Signature);
        }
    }
    auto PreviousDescriptorExists = [](const FString& Key)
    {
        FString Path;
        if (Key.StartsWith(TEXT("project:"))) Path = FPaths::ProjectDir() / Key.Mid(8);
        else if (Key.StartsWith(TEXT("engine:"))) Path = FPaths::EngineDir() / Key.Mid(7);
        else if (Key.StartsWith(TEXT("external:"))) Path = Key.Mid(9);
        else return true;
        return IFileManager::Get().FileExists(*Path);
    };
    bool bChanged = false;
    for (const auto& Pair : DiscoveredFingerprints)
    {
        if (ModFolders.Contains(Pair.Key))
        {
            if (ModFingerprints.FindRef(Pair.Key) != Pair.Value)
            { ModFingerprints.Add(Pair.Key, Pair.Value); bChanged = true; }
            continue;
        }
        if (SignatureCounts.FindChecked(Pair.Value) != 1) continue;
        TArray<FString> Candidates;
        for (const auto& Previous : ModFingerprints)
        {
            if (Previous.Value == Pair.Value && ModFolders.Contains(Previous.Key) &&
                !Descriptors.Contains(Previous.Key) && !PreviousDescriptorExists(Previous.Key)) Candidates.Add(Previous.Key);
        }
        // Never infer a rename from a display name, a changed descriptor, an existing
        // original directory, or an ambiguous set of copies.
        if (Candidates.Num() == 1)
        {
            const FString OldKey = Candidates[0];
            const FString Folder = ModFolders.FindChecked(OldKey);
            ModFolders.Remove(OldKey);
            ModFolders.Add(Pair.Key, Folder);
            ModFingerprints.Remove(OldKey);
            ModFingerprints.Add(Pair.Key, Pair.Value);
            bChanged = true;
        }
    }
    return bChanged;
}

bool FCrimeFactoryModOrganisation::Load(FText& Error)
{
    if (!IFileManager::Get().FileExists(*Filename)) return true;
    FString Text;
    TSharedPtr<FJsonObject> Json;
    double Version = 0;
    const TArray<TSharedPtr<FJsonValue>>* FolderValues = nullptr;
    const TSharedPtr<FJsonObject>* Assignments = nullptr;
    if (!FFileHelper::LoadFileToString(Text, *Filename) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid() ||
        !Json->TryGetNumberField(TEXT("Version"), Version) || Version != 1 ||
        !Json->TryGetArrayField(TEXT("Folders"), FolderValues) || !Json->TryGetObjectField(TEXT("Mods"), Assignments))
    {
        bPreserveInvalidFile = true;
        Error = LOCTEXT("InvalidOrganisation", "CrimeFactory could not read its folder preferences. Mods remain available in All Mods. The original file will be backed up before saving new organisation.");
        return false;
    }

    Folders.Reset();
    ModFolders.Reset();
    ModFingerprints.Reset();
    bool bRepaired = false;
    for (const auto& Value : *FolderValues)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        FCrimeFactoryModFolder Node;
        FGuid Guid;
        if (!Value.IsValid() || !Value->TryGetObject(Object) ||
            !(*Object)->TryGetStringField(TEXT("Id"), Node.Id) || !FGuid::Parse(Node.Id, Guid) ||
            !(*Object)->TryGetStringField(TEXT("Parent"), Node.Parent) ||
            !(*Object)->TryGetStringField(TEXT("Name"), Node.Name) || Folders.Contains(Node.Id))
        { bRepaired = true; continue; }
        Node.Name.TrimStartAndEndInline();
        const FString Id = Node.Id;
        Folders.Add(Id, MoveTemp(Node));
    }
    // Repair references iteratively, including cycles, without recursive traversal.
    for (auto& Pair : Folders)
    {
        FString Cursor = Pair.Value.Parent;
        TSet<FString> Seen { Pair.Key };
        int32 Depth = 0;
        while (!Cursor.IsEmpty())
        {
            const auto* Parent = Find(Cursor);
            if (!Parent || Seen.Contains(Cursor) || ++Depth >= 128)
            { Pair.Value.Parent.Empty(); bRepaired = true; break; }
            Seen.Add(Cursor);
            Cursor = Parent->Parent;
        }
    }
    TArray<FString> Ids;
    Folders.GetKeys(Ids);
    Ids.Sort();
    for (const FString& Id : Ids)
    {
        auto& Node = Folders.FindChecked(Id);
        FText NameError;
        if (!ValidateName(Node.Parent, Node.Name, Id, NameError))
        {
            const FString BaseName = TEXT("Recovered Folder ") + Id;
            Node.Name = BaseName;
            int32 Suffix = 1;
            while (!ValidateName(Node.Parent, Node.Name, Id, NameError)) Node.Name = FString::Printf(TEXT("%s (%d)"), *BaseName, Suffix++);
            bRepaired = true;
        }
    }
    for (const auto& Pair : (*Assignments)->Values)
    {
        FString Folder;
        if (!Pair.Key.IsEmpty() && Pair.Value->TryGetString(Folder) && HasFolder(Folder)) ModFolders.Add(Pair.Key, Folder);
        else bRepaired = true;
    }
    const TSharedPtr<FJsonObject>* Fingerprints = nullptr;
    if (Json->TryGetObjectField(TEXT("DescriptorFingerprints"), Fingerprints))
    {
        for (const auto& Pair : (*Fingerprints)->Values)
        {
            FString Signature;
            if (ModFolders.Contains(Pair.Key) && Pair.Value->TryGetString(Signature)) ModFingerprints.Add(Pair.Key, Signature);
        }
    }
    Json->TryGetStringField(TEXT("CurrentFolder"), CurrentFolder);
    if (CurrentFolder != AllMods && !HasFolder(CurrentFolder)) CurrentFolder = AllMods;
    double Width;
    if (Json->TryGetNumberField(TEXT("SidebarFraction"), Width) && FMath::IsFinite(Width)) SidebarFraction = FMath::Clamp(static_cast<float>(Width), 0.12f, 0.6f);
    Json->TryGetBoolField(TEXT("ShowSidebar"), bShowSidebar);
    const TArray<TSharedPtr<FJsonValue>>* Expanded = nullptr;
    if (Json->TryGetArrayField(TEXT("ExpandedFolders"), Expanded))
    {
        for (const auto& Value : *Expanded)
        {
            FString Id;
            if (Value->TryGetString(Id) && HasFolder(Id)) ExpandedFolders.Add(Id);
        }
    }
    if (bRepaired)
    {
        bPreserveInvalidFile = true;
        Error = LOCTEXT("RepairedOrganisation", "CrimeFactory recovered invalid folder preferences. The original file will be backed up before saving the recovered organisation.");
    }
    return !bRepaired;
}

bool FCrimeFactoryModOrganisation::Save(FText& Error)
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("Version"), 1);
    TArray<FString> Ids;
    Folders.GetKeys(Ids);
    Ids.Sort();
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FString& Id : Ids)
    {
        const auto& Node = Folders.FindChecked(Id);
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("Id"), Id);
        Object->SetStringField(TEXT("Parent"), Node.Parent);
        Object->SetStringField(TEXT("Name"), Node.Name);
        Values.Add(MakeShared<FJsonValueObject>(Object));
    }
    Json->SetArrayField(TEXT("Folders"), Values);
    TSharedRef<FJsonObject> Assignments = MakeShared<FJsonObject>();
    ModFolders.GetKeys(Ids);
    Ids.Sort();
    for (const FString& Key : Ids) Assignments->SetStringField(Key, ModFolders.FindChecked(Key));
    Json->SetObjectField(TEXT("Mods"), Assignments);
    TSharedRef<FJsonObject> Fingerprints = MakeShared<FJsonObject>();
    ModFingerprints.GetKeys(Ids);
    Ids.Sort();
    for (const FString& Key : Ids) Fingerprints->SetStringField(Key, ModFingerprints.FindChecked(Key));
    Json->SetObjectField(TEXT("DescriptorFingerprints"), Fingerprints);
    Json->SetStringField(TEXT("CurrentFolder"), CurrentFolder);
    Json->SetNumberField(TEXT("SidebarFraction"), SidebarFraction);
    Json->SetBoolField(TEXT("ShowSidebar"), bShowSidebar);
    Values.Reset();
    Ids = ExpandedFolders.Array();
    Ids.Sort();
    for (const FString& Id : Ids) Values.Add(MakeShared<FJsonValueString>(Id));
    Json->SetArrayField(TEXT("ExpandedFolders"), Values);
    FString Text;
    FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
    IFileManager& Files = IFileManager::Get();
    if (!Files.MakeDirectory(*FPaths::GetPath(Filename), true))
    { Error = LOCTEXT("CreateConfigFailure", "CrimeFactory could not create its local folder-preferences directory."); return false; }
    if (bPreserveInvalidFile && Files.FileExists(*Filename))
    {
        const FString Backup = Filename + TEXT(".recovered-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        if (Files.Copy(*Backup, *Filename, false) != COPY_OK)
        { Error = LOCTEXT("BackupFailure", "CrimeFactory could not back up invalid folder preferences. The original file was left untouched."); return false; }
        bPreserveInvalidFile = false;
    }
    const FString Temporary = Filename + TEXT(".tmp");
    if (!FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
        !Files.Move(*Filename, *Temporary, true, false, false, true))
    {
        Files.Delete(*Temporary, false, true);
        Error = LOCTEXT("SaveFailure", "CrimeFactory could not save folder preferences. Changes are available for this session; check that the local config file is writable.");
        return false;
    }
    return true;
}

#undef LOCTEXT_NAMESPACE
