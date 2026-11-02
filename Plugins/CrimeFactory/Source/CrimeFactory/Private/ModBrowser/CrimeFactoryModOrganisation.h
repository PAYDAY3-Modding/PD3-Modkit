#pragma once

#include "CoreMinimal.h"

class IPlugin;

/** A virtual folder. IDs remain unchanged when a folder is renamed or moved. */
struct FCrimeFactoryModFolder
{
    FString Id;
    FString Parent;
    FString Name;
};

/** Local browser preferences only. This class never writes mod or plugin files. */
class FCrimeFactoryModOrganisation
{
public:
    explicit FCrimeFactoryModOrganisation(const FString& InFilename = FString());
    bool Load(FText& Error);
    bool Save(FText& Error);

    static FString ModKey(const IPlugin& Plugin);
    static const FString AllMods;
    bool HasFolder(const FString& Id) const { return Id.IsEmpty() || Folders.Contains(Id); }
    const FCrimeFactoryModFolder* Find(const FString& Id) const { return Folders.Find(Id); }
    TArray<FCrimeFactoryModFolder> Children(const FString& Parent) const;
    bool IsWithin(const FString& Folder, const FString& Ancestor) const;
    FString FolderForMod(const FString& Key) const;
    bool ValidateName(const FString& Parent, const FString& Name, const FString& ExceptId, FText& Error) const;
    bool CreateFolder(const FString& Parent, const FString& Name, FString& NewId, FText& Error);
    bool RenameFolder(const FString& Id, const FString& Name, FText& Error);
    bool CanMoveFolder(const FString& Id, const FString& Parent, FText& Error) const;
    bool MoveFolder(const FString& Id, const FString& Parent, FText& Error);
    bool DeleteFolder(const FString& Id);
    bool AssignMods(const TArray<FString>& Keys, const FString& Folder);
    /** Recover unambiguous directory renames using unchanged descriptor contents. */
    bool ReconcileDescriptors(const TMap<FString, FString>& Descriptors);

    FString CurrentFolder = AllMods;
    TSet<FString> ExpandedFolders;
    float SidebarFraction = 0.23f;
    bool bShowSidebar = true;

private:
    FString Filename;
    TMap<FString, FCrimeFactoryModFolder> Folders;
    TMap<FString, FString> ModFolders;
    TMap<FString, FString> ModFingerprints;
    TMap<FString, FString> DiscoveredFingerprints;
    bool bPreserveInvalidFile = false;
};
