#include "LegacyModImporter.h"

#include "CrimeForge.h"
#include "ModWizardDefinition.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "IAssetTools.h"
#include "DesktopPlatformModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PluginDescriptor.h"
#include "PluginUtils.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/CoreRedirects.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UnrealType.h"

namespace
{
    struct FLegacyMovePlan
    {
        FAssetData AssetData;
        UObject* Asset = nullptr;
        FString TargetPackagePath;
        FString TargetObjectPath;
        FString OriginalParentClassPath;
        FString ReplacementParentClassPath;
    };

    struct FLegacyImportAnalysis
    {
        FString OriginalSourcePhysicalPath;
        FString SourcePhysicalPath;
        FString SourceVirtualPath;
        FString ModName;
        bool bStagedExternalCopy = false;

        // Old project information is kept so the importer can also inspect the
        // legacy global ModMeta folder:
        //
        //   /Game/Mods/ModMeta
        FString LegacyContentDirectory;
        FString LegacyModMetaPhysicalPath;
        FString LegacyModMetaVirtualPath = TEXT("/Game/Mods/ModMeta");
        bool bStagedLegacyModMeta = false;

        FAssetData LegacyModMetaAsset;
        FString LegacyModMetaObjectPath;
        int32 LegacyModMetaReferenceScore = 0;
        TArray<FString> LegacyModMetaMatchedReferences;

        TArray<FAssetData> Assets;
        TMap<FString, FString> ClassRedirects;
        TMap<FSoftObjectPath, FSoftObjectPath> AssetRedirects;

        // Legacy mods often referenced reconstructed/dummy base-game assets
        // outside /Game/Mods. Those packages must remain at their original
        // /Game path after migration. If the current PAYDAY3 project already
        // owns the package we reuse it; otherwise we copy the uncooked package
        // from the old project's Content directory into the same /Game path.
        TArray<FString> ExternalGameDependenciesToMigrate;
        TArray<FString> ExternalGameDependenciesReused;
        TArray<FString> ExternalGameDependenciesMissingFromSource;

        int32 BlueprintCount = 0;
        int32 KnownParentRepairs = 0;
        int32 UnknownMissingParents = 0;
        TArray<FString> UnknownParentLines;
    };

    static FString StripExportTextPath(FString Path)
    {
        Path.TrimStartAndEndInline();

        int32 FirstQuote = INDEX_NONE;
        if (Path.FindChar(TEXT('\''), FirstQuote))
        {
            Path = Path.Mid(FirstQuote + 1);
            Path.RemoveFromEnd(TEXT("'"));
        }

        Path.TrimStartAndEndInline();
        return Path;
    }

    static FString GetObjectNameFromObjectPath(const FString& ObjectPath)
    {
        FString Clean = StripExportTextPath(ObjectPath);

        int32 DotIndex = INDEX_NONE;
        if (Clean.FindLastChar(TEXT('.'), DotIndex))
        {
            return Clean.Mid(DotIndex + 1);
        }

        int32 SlashIndex = INDEX_NONE;
        if (Clean.FindLastChar(TEXT('/'), SlashIndex))
        {
            return Clean.Mid(SlashIndex + 1);
        }

        return Clean;
    }

    static FString GetPackageLeafName(const FString& PackageName)
    {
        int32 SlashIndex = INDEX_NONE;
        if (PackageName.FindLastChar(TEXT('/'), SlashIndex))
        {
            return PackageName.Mid(SlashIndex + 1);
        }

        return PackageName;
    }

    static FString NormalizeLegacyName(FString Name)
    {
        Name = GetObjectNameFromObjectPath(Name);
        Name.ToLowerInline();

        Name.RemoveFromEnd(TEXT("_c"));
        Name.RemoveFromStart(TEXT("bp_"));

        return Name;
    }

    static bool IsLikelyLegacyModLoaderPath(const FString& Path)
    {
        const FString Clean = StripExportTextPath(Path);

        return Clean.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase) &&
            (Clean.Contains(TEXT("/Mods/"), ESearchCase::IgnoreCase) ||
             Clean.Contains(TEXT("/ModLoader/"), ESearchCase::IgnoreCase));
    }

    static bool DoesClassPackageExistWithoutLoading(const FString& ClassPath)
    {
        if (ClassPath.IsEmpty())
        {
            return true;
        }

        const FString Clean = StripExportTextPath(ClassPath);

        if (Clean.StartsWith(TEXT("/Script/"), ESearchCase::IgnoreCase))
        {
            return true;
        }

        const FString PackageName = FPackageName::ObjectPathToPackageName(Clean);
        if (!FPackageName::IsValidLongPackageName(PackageName))
        {
            return false;
        }

        // Do not LoadObject() here. A failed load before the migration redirects
        // are installed can poison the current editor session for that legacy
        // path. This disk/package existence check is deliberately non-loading.
        return FPackageName::DoesPackageExist(PackageName);
    }

    static void BuildPDMLTargetMaps(
        IAssetRegistry& AssetRegistry,
        TMap<FString, FString>& OutClassByNormalizedName,
        TMap<FString, FString>& OutAssetByNormalizedName,
        TMap<FString, FString>& OutGeneratedClassByAssetNormalizedName)
    {
        TArray<FString> PathsToScan;
        PathsToScan.Add(TEXT("/PDML"));
        AssetRegistry.ScanPathsSynchronous(PathsToScan, true);

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/PDML")));
        Filter.bRecursivePaths = true;

        TArray<FAssetData> PDMLAssets;
        AssetRegistry.GetAssets(Filter, PDMLAssets);

        for (const FAssetData& AssetData : PDMLAssets)
        {
            const FString AssetObjectPath = AssetData.GetSoftObjectPath().ToString();
            const FString AssetKey = NormalizeLegacyName(AssetData.AssetName.ToString());

            if (!AssetKey.IsEmpty() && !OutAssetByNormalizedName.Contains(AssetKey))
            {
                OutAssetByNormalizedName.Add(AssetKey, AssetObjectPath);
            }

            FString GeneratedClassTag;
            if (AssetData.GetTagValue(FName(TEXT("GeneratedClass")), GeneratedClassTag))
            {
                const FString GeneratedClassPath = StripExportTextPath(GeneratedClassTag);
                const FString ClassKey = NormalizeLegacyName(GeneratedClassPath);

                if (!ClassKey.IsEmpty() && !OutClassByNormalizedName.Contains(ClassKey))
                {
                    OutClassByNormalizedName.Add(ClassKey, GeneratedClassPath);
                }

                if (!AssetKey.IsEmpty())
                {
                    OutGeneratedClassByAssetNormalizedName.Add(AssetKey, GeneratedClassPath);
                }
            }
        }

        // Known PDML classes are kept as explicit fallbacks even if the Asset
        // Registry has not populated their GeneratedClass tag yet.
        const FString ModActorKey = NormalizeLegacyName(TEXT("ModActorBase_C"));
        if (!OutClassByNormalizedName.Contains(ModActorKey))
        {
            OutClassByNormalizedName.Add(
                ModActorKey,
                TEXT("/PDML/Blueprints/ModActorBase.ModActorBase_C"));
        }

        const FString ModMetaKey = NormalizeLegacyName(TEXT("PDA_ModMeta_C"));
        if (!OutClassByNormalizedName.Contains(ModMetaKey))
        {
            OutClassByNormalizedName.Add(
                ModMetaKey,
                TEXT("/PDML/Blueprints/PDA_ModMeta.PDA_ModMeta_C"));
        }
    }

    static FString ResolvePDMLClassTarget(
        const FString& OldClassPath,
        const TMap<FString, FString>& PDMLClasses)
    {
        if (OldClassPath.IsEmpty())
        {
            return FString();
        }

        const FString Key = NormalizeLegacyName(OldClassPath);
        if (const FString* Found = PDMLClasses.Find(Key))
        {
            return *Found;
        }

        return FString();
    }

    static FString ResolvePDMLAssetTarget(
        const FString& OldPackageName,
        const TMap<FString, FString>& PDMLAssets)
    {
        const FString Key = NormalizeLegacyName(GetPackageLeafName(OldPackageName));
        if (const FString* Found = PDMLAssets.Find(Key))
        {
            return *Found;
        }

        return FString();
    }

    static bool IsPathInsideDirectory(
        const FString& CandidatePath,
        const FString& DirectoryPath,
        FString* OutRelativePath = nullptr)
    {
        FString Candidate = FPaths::ConvertRelativePathToFull(CandidatePath);
        FString Directory = FPaths::ConvertRelativePathToFull(DirectoryPath);
        FPaths::NormalizeDirectoryName(Candidate);
        FPaths::NormalizeDirectoryName(Directory);

        const FString Prefix = Directory.EndsWith(TEXT("/"))
            ? Directory
            : Directory + TEXT("/");

        if (!Candidate.StartsWith(Prefix, ESearchCase::IgnoreCase))
        {
            return false;
        }

        if (OutRelativePath)
        {
            *OutRelativePath = Candidate.Mid(Prefix.Len());
            OutRelativePath->ReplaceInline(TEXT("\\"), TEXT("/"));
            OutRelativePath->RemoveFromStart(TEXT("/"));
            OutRelativePath->RemoveFromEnd(TEXT("/"));
        }

        return true;
    }

    static bool FindOwningUnrealContentDirectory(
        const FString& SelectedFolder,
        FString& OutContentDirectory)
    {
        FString Current = FPaths::ConvertRelativePathToFull(SelectedFolder);
        FPaths::NormalizeDirectoryName(Current);

        while (!Current.IsEmpty())
        {
            if (FPaths::GetCleanFilename(Current).Equals(
                    TEXT("Content"),
                    ESearchCase::IgnoreCase))
            {
                const FString ProjectRoot = FPaths::GetPath(Current);
                TArray<FString> ProjectFiles;
                IFileManager::Get().FindFiles(
                    ProjectFiles,
                    *FPaths::Combine(ProjectRoot, TEXT("*.uproject")),
                    true,
                    false);

                if (ProjectFiles.Num() > 0)
                {
                    OutContentDirectory = Current;
                    return true;
                }
            }

            const FString Parent = FPaths::GetPath(Current);
            if (Parent.IsEmpty() || Parent.Equals(Current, ESearchCase::IgnoreCase))
            {
                break;
            }

            Current = Parent;
        }

        return false;
    }

    static FString NormalizeMetadataPropertyName(
        const FProperty* Property)
    {
        if (!Property)
        {
            return FString();
        }

        FString Name = Property->GetDisplayNameText().ToString();
        if (Name.IsEmpty())
        {
            Name = Property->GetName();
        }

        Name.ToLowerInline();
        Name.ReplaceInline(TEXT(" "), TEXT(""));
        Name.ReplaceInline(TEXT("_"), TEXT(""));
        Name.ReplaceInline(TEXT("-"), TEXT(""));
        return Name;
    }

    static bool ShouldSkipLegacyMetadataProperty(
        const FProperty* Property)
    {
        const FString Key =
            NormalizeMetadataPropertyName(Property);

        // These fields must reflect the new CrimeForge/PDML mod rather than the
        // legacy loader. Mod Actors is rebuilt from the migrated Blueprint
        // classes by PopulateGeneratedModDataAssetForPlugin().
        return
            Key == TEXT("modid") ||
            Key == TEXT("modenvironment") ||
            Key == TEXT("modactors") ||
            Key == TEXT("modloaderversion");
    }

    static int32 CopyCompatibleLegacyMetadataProperties(
        UObject* LegacyMetadata,
        UObject* GeneratedMetadata,
        TArray<FString>& OutCopiedProperties,
        TArray<FString>& OutSkippedProperties)
    {
        if (!LegacyMetadata || !GeneratedMetadata)
        {
            return 0;
        }

        TMap<FString, FProperty*> TargetProperties;

        for (TFieldIterator<FProperty> It(
                 GeneratedMetadata->GetClass(),
                 EFieldIteratorFlags::IncludeSuper);
             It;
             ++It)
        {
            FProperty* Property = *It;

            if (!Property ||
                Property->HasAnyPropertyFlags(
                    CPF_Transient |
                    CPF_DuplicateTransient |
                    CPF_NonPIEDuplicateTransient))
            {
                continue;
            }

            const FString Key =
                NormalizeMetadataPropertyName(Property);

            if (!Key.IsEmpty())
            {
                TargetProperties.FindOrAdd(Key) = Property;
            }
        }

        int32 CopiedCount = 0;

        for (TFieldIterator<FProperty> It(
                 LegacyMetadata->GetClass(),
                 EFieldIteratorFlags::IncludeSuper);
             It;
             ++It)
        {
            FProperty* SourceProperty = *It;

            if (!SourceProperty ||
                SourceProperty->HasAnyPropertyFlags(
                    CPF_Transient |
                    CPF_DuplicateTransient |
                    CPF_NonPIEDuplicateTransient) ||
                ShouldSkipLegacyMetadataProperty(SourceProperty))
            {
                continue;
            }

            const FString Key =
                NormalizeMetadataPropertyName(SourceProperty);

            FProperty** TargetPropertyPtr =
                TargetProperties.Find(Key);

            if (!TargetPropertyPtr ||
                !*TargetPropertyPtr)
            {
                continue;
            }

            FProperty* TargetProperty =
                *TargetPropertyPtr;

            if (ShouldSkipLegacyMetadataProperty(TargetProperty))
            {
                continue;
            }

            /*
             * Only copy properties whose serialized type is genuinely the
             * same. This safely preserves fields such as name/version/authors/
             * description and any unchanged PDML data fields, while refusing
             * to reinterpret structs/classes whose schema changed between the
             * UE4 loader and PDML.
             */
            if (!TargetProperty->SameType(SourceProperty))
            {
                OutSkippedProperties.AddUnique(
                    SourceProperty->GetDisplayNameText().ToString());
                continue;
            }

            const void* SourceValue =
                SourceProperty->ContainerPtrToValuePtr<void>(
                    LegacyMetadata);

            void* TargetValue =
                TargetProperty->ContainerPtrToValuePtr<void>(
                    GeneratedMetadata);

            if (!SourceValue || !TargetValue)
            {
                continue;
            }

            GeneratedMetadata->Modify();

            TargetProperty->CopyCompleteValue(
                TargetValue,
                SourceValue);

            OutCopiedProperties.AddUnique(
                TargetProperty->GetDisplayNameText().ToString());

            ++CopiedCount;
        }

        if (CopiedCount > 0)
        {
            GeneratedMetadata->MarkPackageDirty();
        }

        return CopiedCount;
    }

    static bool IsExternalLegacyGamePackage(
        const FString& PackageName,
        const FString& SelectedModRoot)
    {
        if (!PackageName.StartsWith(
                TEXT("/Game/"),
                ESearchCase::IgnoreCase))
        {
            return false;
        }

        FString SelectedPrefix = SelectedModRoot;
        if (!SelectedPrefix.EndsWith(TEXT("/")))
        {
            SelectedPrefix += TEXT("/");
        }

        if (PackageName.Equals(
                SelectedModRoot,
                ESearchCase::IgnoreCase) ||
            PackageName.StartsWith(
                SelectedPrefix,
                ESearchCase::IgnoreCase))
        {
            return false;
        }

        /*
         * Anything under the legacy /Mods or /ModLoader trees is handled by
         * the normal selected-mod migration / PDML redirect logic. The special
         * dependency migration here is specifically for reconstructed game
         * assets that must stay at their original /Game/... path.
         */
        if (PackageName.Contains(
                TEXT("/Mods/"),
                ESearchCase::IgnoreCase) ||
            PackageName.Contains(
                TEXT("/ModLoader/"),
                ESearchCase::IgnoreCase))
        {
            return false;
        }

        return true;
    }

    static FString GetLegacyPackageBaseFilename(
        const FString& LegacyContentDirectory,
        const FString& PackageName)
    {
        if (LegacyContentDirectory.IsEmpty() ||
            !PackageName.StartsWith(
                TEXT("/Game/"),
                ESearchCase::IgnoreCase))
        {
            return FString();
        }

        FString RelativePackage =
            PackageName.Mid(6); // strip "/Game/"

        RelativePackage.ReplaceInline(
            TEXT("/"),
            TEXT("/"));

        return FPaths::ConvertRelativePathToFull(
            FPaths::Combine(
                LegacyContentDirectory,
                RelativePackage));
    }

    static bool FindLegacyPackageFile(
        const FString& LegacyContentDirectory,
        const FString& PackageName,
        FString& OutPackageFile)
    {
        OutPackageFile.Reset();

        const FString BaseFilename =
            GetLegacyPackageBaseFilename(
                LegacyContentDirectory,
                PackageName);

        if (BaseFilename.IsEmpty())
        {
            return false;
        }

        const FString AssetFile =
            BaseFilename + TEXT(".uasset");

        if (IFileManager::Get().FileExists(
                *AssetFile))
        {
            OutPackageFile = AssetFile;
            return true;
        }

        const FString MapFile =
            BaseFilename + TEXT(".umap");

        if (IFileManager::Get().FileExists(
                *MapFile))
        {
            OutPackageFile = MapFile;
            return true;
        }

        return false;
    }

    static void AnalyzeExternalGameDependencies(
        FLegacyImportAnalysis& Analysis,
        IAssetRegistry& AssetRegistry)
    {
        TSet<FString> SeenPackages;

        for (const FAssetData& AssetData : Analysis.Assets)
        {
            TArray<FName> Dependencies;
            AssetRegistry.GetDependencies(
                AssetData.PackageName,
                Dependencies,
                UE::AssetRegistry::EDependencyCategory::Package);

            for (const FName DependencyName : Dependencies)
            {
                const FString DependencyPackage =
                    DependencyName.ToString();

                if (!IsExternalLegacyGamePackage(
                        DependencyPackage,
                        Analysis.SourceVirtualPath) ||
                    SeenPackages.Contains(
                        DependencyPackage))
                {
                    continue;
                }

                SeenPackages.Add(
                    DependencyPackage);

                /*
                 * If PAYDAY3 already has the dummy/reconstructed package, keep
                 * the reference exactly as-is and use that existing asset.
                 */
                if (FPackageName::DoesPackageExist(
                        DependencyPackage))
                {
                    Analysis.ExternalGameDependenciesReused.Add(
                        DependencyPackage);
                    continue;
                }

                FString LegacyPackageFile;
                if (FindLegacyPackageFile(
                        Analysis.LegacyContentDirectory,
                        DependencyPackage,
                        LegacyPackageFile))
                {
                    Analysis.ExternalGameDependenciesToMigrate.Add(
                        DependencyPackage);
                }
                else
                {
                    Analysis.ExternalGameDependenciesMissingFromSource.Add(
                        DependencyPackage);
                }
            }
        }

        Analysis.ExternalGameDependenciesToMigrate.Sort();
        Analysis.ExternalGameDependenciesReused.Sort();
        Analysis.ExternalGameDependenciesMissingFromSource.Sort();
    }

    static bool CopyLegacyPackageFileSet(
        const FString& SourcePackageFile,
        const FString& DestinationPackageFile,
        TArray<FString>& OutCopiedFiles,
        FString& OutFailure)
    {
        OutFailure.Reset();

        const FString SourceBase =
            FPaths::ChangeExtension(
                SourcePackageFile,
                TEXT(""));

        const FString DestinationBase =
            FPaths::ChangeExtension(
                DestinationPackageFile,
                TEXT(""));

        const FString MainExtension =
            FPaths::GetExtension(
                SourcePackageFile,
                true);

        static const TCHAR* SidecarExtensions[] =
        {
            TEXT(".uexp"),
            TEXT(".ubulk"),
            TEXT(".uptnl")
        };

        IFileManager::Get().MakeDirectory(
            *FPaths::GetPath(DestinationPackageFile),
            true);

        auto CopyOne =
            [&OutCopiedFiles, &OutFailure](
                const FString& SourceFile,
                const FString& DestinationFile,
                const bool bRequired)
            {
                if (!IFileManager::Get().FileExists(
                        *SourceFile))
                {
                    if (bRequired)
                    {
                        OutFailure = FString::Printf(
                            TEXT("Source package file does not exist: %s"),
                            *SourceFile);
                        return false;
                    }

                    return true;
                }

                // Never replace a reconstructed/current PAYDAY3 asset.
                if (IFileManager::Get().FileExists(
                        *DestinationFile))
                {
                    return true;
                }

                if (IFileManager::Get().Copy(
                        *DestinationFile,
                        *SourceFile,
                        false,
                        true) != COPY_OK)
                {
                    OutFailure = FString::Printf(
                        TEXT("Failed to copy '%s' to '%s'"),
                        *SourceFile,
                        *DestinationFile);
                    return false;
                }

                OutCopiedFiles.Add(
                    DestinationFile);
                return true;
            };

        if (!CopyOne(
                SourceBase + MainExtension,
                DestinationBase + MainExtension,
                true))
        {
            return false;
        }

        for (const TCHAR* Extension : SidecarExtensions)
        {
            if (!CopyOne(
                    SourceBase + Extension,
                    DestinationBase + Extension,
                    false))
            {
                return false;
            }
        }

        return true;
    }

    static void MigrateExternalGameDependencies(
        const FLegacyImportAnalysis& Analysis,
        IAssetRegistry& AssetRegistry,
        TArray<FString>& OutMigratedPackages,
        TArray<FString>& OutReusedPackages,
        TArray<FString>& OutFailures)
    {
        OutReusedPackages =
            Analysis.ExternalGameDependenciesReused;

        TArray<FString> PendingPackages =
            Analysis.ExternalGameDependenciesToMigrate;

        TSet<FString> QueuedPackages;
        TSet<FString> ProcessedPackages;

        for (const FString& PackageName : PendingPackages)
        {
            QueuedPackages.Add(PackageName);
        }

        /*
         * Build the complete external /Game dependency closure before loading
         * a single legacy mod asset.
         *
         * If a ModActor is loaded while one of its reconstructed /Game
         * references is absent, UE can deserialize that reference as null.
         * Saving the migrated Blueprint afterwards then permanently writes the
         * lost reference.
         *
         * Copy -> scan metadata -> inspect dependencies -> queue more missing
         * /Game packages, until nothing is left.
         */
        int32 PendingIndex = 0;

        while (PendingIndex < PendingPackages.Num())
        {
            const FString PackageName =
                PendingPackages[PendingIndex++];

            if (ProcessedPackages.Contains(PackageName))
            {
                continue;
            }

            ProcessedPackages.Add(PackageName);

            if (FPackageName::DoesPackageExist(PackageName))
            {
                OutReusedPackages.AddUnique(PackageName);
                continue;
            }

            FString SourcePackageFile;
            if (!FindLegacyPackageFile(
                    Analysis.LegacyContentDirectory,
                    PackageName,
                    SourcePackageFile))
            {
                OutFailures.AddUnique(
                    FString::Printf(
                        TEXT("%s (not found in legacy project)"),
                        *PackageName));
                continue;
            }

            const FString RelativePackage =
                PackageName.Mid(6); // strip /Game/

            const FString DestinationPackageFile =
                FPaths::ConvertRelativePathToFull(
                    FPaths::Combine(
                        FPaths::ProjectContentDir(),
                        RelativePackage +
                        FPaths::GetExtension(
                            SourcePackageFile,
                            true)));

            TArray<FString> CopiedFiles;
            FString CopyFailure;

            if (!CopyLegacyPackageFileSet(
                    SourcePackageFile,
                    DestinationPackageFile,
                    CopiedFiles,
                    CopyFailure))
            {
                OutFailures.AddUnique(
                    FString::Printf(
                        TEXT("%s (%s)"),
                        *PackageName,
                        *CopyFailure));
                continue;
            }

            OutMigratedPackages.AddUnique(PackageName);

            UE_LOG(
                LogCrimeForge,
                Display,
                TEXT(
                    "Legacy importer migrated missing game dependency "
                    "in-place before loading mod assets: %s"
                ),
                *PackageName);

            const FString PackagePath =
                FPackageName::GetLongPackagePath(PackageName);

            if (!PackagePath.IsEmpty())
            {
                TArray<FString> PathsToScan;
                PathsToScan.Add(PackagePath);

                AssetRegistry.ScanPathsSynchronous(
                    PathsToScan,
                    true);
            }

            TArray<FName> Dependencies;
            AssetRegistry.GetDependencies(
                FName(*PackageName),
                Dependencies,
                UE::AssetRegistry::EDependencyCategory::Package);

            for (const FName DependencyName : Dependencies)
            {
                const FString DependencyPackage =
                    DependencyName.ToString();

                if (!IsExternalLegacyGamePackage(
                        DependencyPackage,
                        Analysis.SourceVirtualPath) ||
                    ProcessedPackages.Contains(
                        DependencyPackage) ||
                    QueuedPackages.Contains(
                        DependencyPackage))
                {
                    continue;
                }

                if (FPackageName::DoesPackageExist(
                        DependencyPackage))
                {
                    OutReusedPackages.AddUnique(
                        DependencyPackage);
                    continue;
                }

                FString LegacyDependencyFile;
                if (FindLegacyPackageFile(
                        Analysis.LegacyContentDirectory,
                        DependencyPackage,
                        LegacyDependencyFile))
                {
                    PendingPackages.Add(
                        DependencyPackage);
                    QueuedPackages.Add(
                        DependencyPackage);
                }
                else
                {
                    OutFailures.AddUnique(
                        FString::Printf(
                            TEXT(
                                "%s (dependency of %s; missing from both "
                                "PAYDAY3 and legacy project)"
                            ),
                            *DependencyPackage,
                            *PackageName));
                }
            }
        }

        for (const FString& MissingPackage :
             Analysis.ExternalGameDependenciesMissingFromSource)
        {
            if (!FPackageName::DoesPackageExist(MissingPackage))
            {
                OutFailures.AddUnique(
                    FString::Printf(
                        TEXT(
                            "%s (missing from both PAYDAY3 and legacy project)"
                        ),
                        *MissingPackage));
            }
            else
            {
                OutReusedPackages.AddUnique(MissingPackage);
            }
        }

        OutMigratedPackages.Sort();
        OutReusedPackages.Sort();
        OutFailures.Sort();
    }

    static void PrepareMigratedExternalGameDependencies(
        IAssetRegistry& AssetRegistry,
        const TArray<FString>& MigratedPackages,
        TArray<FString>& OutPreparedPackages,
        TArray<FString>& OutPreparationFailures)
    {
        if (MigratedPackages.Num() == 0)
        {
            return;
        }

        UEditorAssetSubsystem* EditorAssetSubsystem =
            GEditor
                ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
                : nullptr;

        TArray<UObject*> AssetsToSave;

        /*
         * Force the newly copied /Game assets to load, compile and save BEFORE
         * the legacy ModMeta or any legacy ModActor is loaded.
         */
        for (const FString& PackageName : MigratedPackages)
        {
            FARFilter PackageFilter;
            PackageFilter.PackageNames.Add(FName(*PackageName));

            TArray<FAssetData> PackageAssets;
            AssetRegistry.GetAssets(
                PackageFilter,
                PackageAssets);

            if (PackageAssets.Num() == 0)
            {
                const FString PackagePath =
                    FPackageName::GetLongPackagePath(PackageName);

                if (!PackagePath.IsEmpty())
                {
                    TArray<FString> PathsToScan;
                    PathsToScan.Add(PackagePath);

                    AssetRegistry.ScanPathsSynchronous(
                        PathsToScan,
                        true);

                    AssetRegistry.GetAssets(
                        PackageFilter,
                        PackageAssets);
                }
            }

            bool bPreparedAnyAsset = false;

            for (const FAssetData& AssetData : PackageAssets)
            {
                UObject* Asset = AssetData.GetAsset();

                if (!Asset)
                {
                    continue;
                }

                bPreparedAnyAsset = true;

                if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
                {
                    FKismetEditorUtilities::CompileBlueprint(Blueprint);

                    if (Blueprint->Status == BS_Error)
                    {
                        OutPreparationFailures.AddUnique(
                            FString::Printf(
                                TEXT("%s (Blueprint compile failed)"),
                                *Asset->GetPathName()));
                    }
                }

                Asset->MarkPackageDirty();
                AssetsToSave.AddUnique(Asset);
            }

            if (bPreparedAnyAsset)
            {
                OutPreparedPackages.AddUnique(PackageName);
            }
            else
            {
                OutPreparationFailures.AddUnique(
                    FString::Printf(
                        TEXT(
                            "%s (copied, but no loadable asset was found "
                            "before legacy mod load)"
                        ),
                        *PackageName));
            }
        }

        if (EditorAssetSubsystem &&
            AssetsToSave.Num() > 0)
        {
            EditorAssetSubsystem->SaveLoadedAssets(
                AssetsToSave,
                false);
        }

        OutPreparedPackages.Sort();
        OutPreparationFailures.Sort();

        UE_LOG(
            LogCrimeForge,
            Display,
            TEXT(
                "Legacy importer prepared %d migrated /Game package(s) "
                "before loading legacy mod assets."
            ),
            OutPreparedPackages.Num());
    }


    static bool PrepareLegacyModMetaSource(
        FLegacyImportAnalysis& Analysis)
    {
        if (Analysis.LegacyContentDirectory.IsEmpty())
        {
            return false;
        }

        const FString LegacyModMetaSource =
            FPaths::ConvertRelativePathToFull(
                FPaths::Combine(
                    Analysis.LegacyContentDirectory,
                    TEXT("Mods"),
                    TEXT("ModMeta")));

        if (!IFileManager::Get().DirectoryExists(
                *LegacyModMetaSource))
        {
            return false;
        }

        /*
         * If the selected mod is already inside this PAYDAY3 project's
         * Content tree, /Game/Mods/ModMeta is already directly accessible.
         */
        const FString CurrentContentDir =
            FPaths::ConvertRelativePathToFull(
                FPaths::ProjectContentDir());

        FString RelativeToCurrent;
        if (IsPathInsideDirectory(
                LegacyModMetaSource,
                CurrentContentDir,
                &RelativeToCurrent))
        {
            Analysis.LegacyModMetaPhysicalPath =
                LegacyModMetaSource;

            Analysis.bStagedLegacyModMeta = false;
            return true;
        }

        /*
         * External legacy project: stage only the old global ModMeta folder in
         * Saved, then mount that scratch folder at its historical package path.
         * This lets Asset Registry inspect the original metadata assets without
         * copying them into the live PAYDAY3 Content directory.
         */
        Analysis.LegacyModMetaPhysicalPath =
            FPaths::ConvertRelativePathToFull(
                FPaths::Combine(
                    FPaths::ProjectSavedDir(),
                    TEXT("CrimeForge"),
                    TEXT("LegacyStaging"),
                    Analysis.ModName + TEXT("_ModMeta")));

        FPaths::NormalizeDirectoryName(
            Analysis.LegacyModMetaPhysicalPath);

        IPlatformFile& PlatformFile =
            FPlatformFileManager::Get().GetPlatformFile();

        if (IFileManager::Get().DirectoryExists(
                *Analysis.LegacyModMetaPhysicalPath))
        {
            PlatformFile.DeleteDirectoryRecursively(
                *Analysis.LegacyModMetaPhysicalPath);
        }

        if (!PlatformFile.CopyDirectoryTree(
                *Analysis.LegacyModMetaPhysicalPath,
                *LegacyModMetaSource,
                false))
        {
            PlatformFile.DeleteDirectoryRecursively(
                *Analysis.LegacyModMetaPhysicalPath);

            Analysis.LegacyModMetaPhysicalPath.Reset();
            return false;
        }

        FString MountRoot =
            Analysis.LegacyModMetaVirtualPath + TEXT("/");

        FString PhysicalRoot =
            Analysis.LegacyModMetaPhysicalPath;

        if (!PhysicalRoot.EndsWith(TEXT("/")))
        {
            PhysicalRoot += TEXT("/");
        }

        FPackageName::RegisterMountPoint(
            MountRoot,
            PhysicalRoot);

        Analysis.bStagedLegacyModMeta = true;

        UE_LOG(
            LogCrimeForge,
            Display,
            TEXT(
                "Legacy importer staged old ModMeta folder '%s' at '%s'"
            ),
            *LegacyModMetaSource,
            *Analysis.LegacyModMetaVirtualPath);

        return true;
    }

    static void AnalyzeLegacyModMetaCandidateRedirects(
        FLegacyImportAnalysis& Analysis,
        IAssetRegistry& AssetRegistry,
        const TMap<FString, FString>& PDMLClasses,
        const TMap<FString, FString>& PDMLAssets,
        const TMap<FString, FString>& PDMLGeneratedClassesByAsset)
    {
        if (!Analysis.LegacyModMetaAsset.IsValid())
        {
            return;
        }

        const FAssetData& AssetData =
            Analysis.LegacyModMetaAsset;

        FString ParentClassTag;
        if (AssetData.GetTagValue(
                FName(TEXT("ParentClass")),
                ParentClassTag) ||
            AssetData.GetTagValue(
                FName(TEXT("NativeParentClass")),
                ParentClassTag))
        {
            const FString OldParent =
                StripExportTextPath(ParentClassTag);

            const FString NewParent =
                ResolvePDMLClassTarget(
                    OldParent,
                    PDMLClasses);

            if (!NewParent.IsEmpty() &&
                !OldParent.Equals(
                    NewParent,
                    ESearchCase::IgnoreCase))
            {
                Analysis.ClassRedirects.Add(
                    OldParent,
                    NewParent);
            }
        }

        const FString AssetClassPath =
            AssetData.AssetClassPath.ToString();

        if (IsLikelyLegacyModLoaderPath(
                AssetClassPath))
        {
            const FString NewClass =
                ResolvePDMLClassTarget(
                    AssetClassPath,
                    PDMLClasses);

            if (!NewClass.IsEmpty())
            {
                Analysis.ClassRedirects.Add(
                    AssetClassPath,
                    NewClass);
            }
        }

        TArray<FName> Dependencies;
        AssetRegistry.GetDependencies(
            AssetData.PackageName,
            Dependencies,
            UE::AssetRegistry::EDependencyCategory::Package);

        for (const FName DependencyName : Dependencies)
        {
            const FString DependencyPackage =
                DependencyName.ToString();

            if (!IsLikelyLegacyModLoaderPath(
                    DependencyPackage) ||
                FPackageName::DoesPackageExist(
                    DependencyPackage))
            {
                continue;
            }

            const FString PDMLAssetTarget =
                ResolvePDMLAssetTarget(
                    DependencyPackage,
                    PDMLAssets);

            if (PDMLAssetTarget.IsEmpty())
            {
                continue;
            }

            const FString OldAssetName =
                GetPackageLeafName(
                    DependencyPackage);

            const FSoftObjectPath OldAssetPath(
                DependencyPackage +
                TEXT(".") +
                OldAssetName);

            Analysis.AssetRedirects.Add(
                OldAssetPath,
                FSoftObjectPath(PDMLAssetTarget));

            const FString NormalizedAssetName =
                NormalizeLegacyName(
                    OldAssetName);

            if (const FString* NewGeneratedClass =
                    PDMLGeneratedClassesByAsset.Find(
                        NormalizedAssetName))
            {
                Analysis.ClassRedirects.Add(
                    DependencyPackage +
                    TEXT(".") +
                    OldAssetName +
                    TEXT("_C"),
                    *NewGeneratedClass);
            }
        }
    }

    static void FindBestLegacyModMetaAsset(
        FLegacyImportAnalysis& Analysis,
        IAssetRegistry& AssetRegistry)
    {
        if (Analysis.LegacyModMetaPhysicalPath.IsEmpty())
        {
            return;
        }

        TArray<FString> PathsToScan;
        PathsToScan.Add(
            Analysis.LegacyModMetaVirtualPath);

        AssetRegistry.ScanPathsSynchronous(
            PathsToScan,
            true);

        FARFilter Filter;
        Filter.PackagePaths.Add(
            FName(*Analysis.LegacyModMetaVirtualPath));
        Filter.bRecursivePaths = true;

        TArray<FAssetData> Candidates;
        AssetRegistry.GetAssets(
            Filter,
            Candidates);

        const FString SourcePrefix =
            Analysis.SourceVirtualPath.EndsWith(
                TEXT("/"))
            ? Analysis.SourceVirtualPath
            : Analysis.SourceVirtualPath + TEXT("/");

        const FString NormalizedModName =
            NormalizeLegacyName(
                Analysis.ModName);

        int32 BestWeightedScore = 0;

        for (const FAssetData& Candidate : Candidates)
        {
            if (Candidate.AssetClassPath ==
                UObjectRedirector::StaticClass()->GetClassPathName())
            {
                continue;
            }

            TArray<FName> Dependencies;
            AssetRegistry.GetDependencies(
                Candidate.PackageName,
                Dependencies,
                UE::AssetRegistry::EDependencyCategory::Package);

            TArray<FString> MatchingReferences;

            for (const FName DependencyName : Dependencies)
            {
                const FString DependencyPackage =
                    DependencyName.ToString();

                if (DependencyPackage.StartsWith(
                        SourcePrefix,
                        ESearchCase::IgnoreCase))
                {
                    MatchingReferences.AddUnique(
                        DependencyPackage);
                }
            }

            if (MatchingReferences.Num() == 0)
            {
                continue;
            }

            /*
             * References into the selected mod are the primary signal.
             * Asset-name similarity is only a tie-breaker.
             */
            int32 WeightedScore =
                MatchingReferences.Num() * 100;

            const FString NormalizedCandidateName =
                NormalizeLegacyName(
                    Candidate.AssetName.ToString());

            if (!NormalizedModName.IsEmpty() &&
                (NormalizedCandidateName.Contains(
                     NormalizedModName,
                     ESearchCase::IgnoreCase) ||
                 NormalizedModName.Contains(
                     NormalizedCandidateName,
                     ESearchCase::IgnoreCase)))
            {
                WeightedScore += 25;
            }

            if (WeightedScore <= BestWeightedScore)
            {
                continue;
            }

            BestWeightedScore =
                WeightedScore;

            Analysis.LegacyModMetaAsset =
                Candidate;

            Analysis.LegacyModMetaObjectPath =
                Candidate.GetSoftObjectPath().ToString();

            Analysis.LegacyModMetaReferenceScore =
                MatchingReferences.Num();

            Analysis.LegacyModMetaMatchedReferences =
                MoveTemp(MatchingReferences);
        }

        Analysis.LegacyModMetaMatchedReferences.Sort();

        if (!Analysis.LegacyModMetaObjectPath.IsEmpty())
        {
            UE_LOG(
                LogCrimeForge,
                Display,
                TEXT(
                    "Legacy importer matched ModMeta '%s' to '%s' using %d "
                    "reference(s) into the selected mod."
                ),
                *Analysis.LegacyModMetaObjectPath,
                *Analysis.ModName,
                Analysis.LegacyModMetaReferenceScore);
        }
    }

    static bool CleanupStagedLegacyCopy(
        const FLegacyImportAnalysis& Analysis)
    {
        bool bAllRemoved = true;

        auto CleanupScratchMount =
            [&bAllRemoved](
                const FString& VirtualPath,
                const FString& PhysicalPath)
            {
                if (VirtualPath.IsEmpty() ||
                    PhysicalPath.IsEmpty())
                {
                    return;
                }

                FString MountRoot = VirtualPath;
                if (!MountRoot.EndsWith(TEXT("/")))
                {
                    MountRoot += TEXT("/");
                }

                FString PhysicalRoot = PhysicalPath;
                if (!PhysicalRoot.EndsWith(TEXT("/")))
                {
                    PhysicalRoot += TEXT("/");
                }

                if (FModuleManager::Get().IsModuleLoaded(
                        TEXT("AssetRegistry")))
                {
                    FAssetRegistryModule& AssetRegistryModule =
                        FModuleManager::GetModuleChecked<FAssetRegistryModule>(
                            TEXT("AssetRegistry"));

                    AssetRegistryModule.Get().RemovePath(
                        VirtualPath);
                }

                FPackageName::UnRegisterMountPoint(
                    MountRoot,
                    PhysicalRoot);

                IPlatformFile& PlatformFile =
                    FPlatformFileManager::Get().GetPlatformFile();

                if (IFileManager::Get().DirectoryExists(
                        *PhysicalPath))
                {
                    PlatformFile.DeleteDirectoryRecursively(
                        *PhysicalPath);
                }

                if (IFileManager::Get().DirectoryExists(
                        *PhysicalPath))
                {
                    bAllRemoved = false;
                }
            };

        if (Analysis.bStagedLegacyModMeta)
        {
            CleanupScratchMount(
                Analysis.LegacyModMetaVirtualPath,
                Analysis.LegacyModMetaPhysicalPath);
        }

        if (Analysis.bStagedExternalCopy)
        {
            CleanupScratchMount(
                Analysis.SourceVirtualPath,
                Analysis.SourcePhysicalPath);
        }

        return bAllRemoved;
    }

    static bool PrepareLegacySource(
        const FString& SelectedFolder,
        FLegacyImportAnalysis& Analysis,
        FString& OutError)
    {
        Analysis.OriginalSourcePhysicalPath =
            FPaths::ConvertRelativePathToFull(SelectedFolder);
        FPaths::NormalizeDirectoryName(Analysis.OriginalSourcePhysicalPath);

        const FString CurrentContentDir =
            FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());

        FString RelativePath;
        if (IsPathInsideDirectory(
                Analysis.OriginalSourcePhysicalPath,
                CurrentContentDir,
                &RelativePath))
        {
            if (RelativePath.IsEmpty())
            {
                OutError = TEXT("Select the legacy mod folder itself, not the current project's Content root.");
                return false;
            }

            Analysis.SourcePhysicalPath = Analysis.OriginalSourcePhysicalPath;
            Analysis.SourceVirtualPath = TEXT("/Game/") + RelativePath;
            Analysis.LegacyContentDirectory = CurrentContentDir;
            Analysis.bStagedExternalCopy = false;
            return true;
        }

        FString OldProjectContentDir;
        if (!FindOwningUnrealContentDirectory(
                Analysis.OriginalSourcePhysicalPath,
                OldProjectContentDir))
        {
            OutError = FString::Printf(
                TEXT(
                    "The selected folder is outside this project and CrimeForge could not find its old Unreal project Content root.\n\n"
                    "Select a mod folder that is either inside this PAYDAY3 project's Content folder, or inside an old Unreal project's Content folder.\n\nSelected:\n%s"),
                *Analysis.OriginalSourcePhysicalPath);
            return false;
        }

        if (!IsPathInsideDirectory(
                Analysis.OriginalSourcePhysicalPath,
                OldProjectContentDir,
                &RelativePath) ||
            RelativePath.IsEmpty())
        {
            OutError = TEXT("Could not calculate the selected mod folder's path relative to the old project's Content directory.");
            return false;
        }

        Analysis.SourceVirtualPath = TEXT("/Game/") + RelativePath;
        Analysis.LegacyContentDirectory = OldProjectContentDir;
        Analysis.bStagedExternalCopy = true;

        /*
         * Do not copy the old mod into this project's Content directory.
         * Doing that makes the temporary packages part of the live /Game
         * content tree and later deleting their redirectors through editor
         * APIs is unsafe in UE 5.5.
         *
         * Instead, copy to Saved and mount that scratch directory at the
         * precise legacy virtual path. The assets still deserialize with their
         * original /Game/... package names, but the scratch files are isolated
         * from the Content Browser and can be deleted physically afterwards.
         */
        const FString ExistingProjectPath =
            FPaths::ConvertRelativePathToFull(
                FPaths::Combine(
                    FPaths::ProjectContentDir(),
                    RelativePath));

        if (IFileManager::Get().DirectoryExists(
                *ExistingProjectPath))
        {
            OutError = FString::Printf(
                TEXT(
                    "The legacy virtual path already exists in this PAYDAY3 "
                    "project, so CrimeForge cannot temporarily mount the external "
                    "source there without ambiguity.\n\nExisting project path:\n"
                    "%s\n\nImport from that in-project copy instead, or move/"
                    "rename the existing folder first."),
                *ExistingProjectPath);
            return false;
        }

        Analysis.SourcePhysicalPath =
            FPaths::ConvertRelativePathToFull(
                FPaths::Combine(
                    FPaths::ProjectSavedDir(),
                    TEXT("CrimeForge"),
                    TEXT("LegacyStaging"),
                    Analysis.ModName));

        FPaths::NormalizeDirectoryName(
            Analysis.SourcePhysicalPath);

        IPlatformFile& PlatformFile =
            FPlatformFileManager::Get().GetPlatformFile();

        // A previous interrupted/crashed migration may have left scratch data.
        if (IFileManager::Get().DirectoryExists(
                *Analysis.SourcePhysicalPath))
        {
            PlatformFile.DeleteDirectoryRecursively(
                *Analysis.SourcePhysicalPath);
        }

        if (!PlatformFile.CopyDirectoryTree(
                *Analysis.SourcePhysicalPath,
                *Analysis.OriginalSourcePhysicalPath,
                false))
        {
            PlatformFile.DeleteDirectoryRecursively(
                *Analysis.SourcePhysicalPath);

            OutError = FString::Printf(
                TEXT(
                    "Failed to create the temporary legacy staging copy:\n%s"),
                *Analysis.SourcePhysicalPath);
            return false;
        }

        FString MountRoot = Analysis.SourceVirtualPath;
        if (!MountRoot.EndsWith(TEXT("/")))
        {
            MountRoot += TEXT("/");
        }

        FString PhysicalRoot = Analysis.SourcePhysicalPath;
        if (!PhysicalRoot.EndsWith(TEXT("/")))
        {
            PhysicalRoot += TEXT("/");
        }

        FPackageName::RegisterMountPoint(
            MountRoot,
            PhysicalRoot);

        UE_LOG(
            LogCrimeForge,
            Display,
            TEXT(
                "Legacy importer staged external source '%s' at scratch path "
                "'%s' and mounted it as '%s'"
            ),
            *Analysis.OriginalSourcePhysicalPath,
            *Analysis.SourcePhysicalPath,
            *MountRoot);

        return true;
    }

    static bool AnalyzeLegacyFolder(
        const FString& SelectedFolder,
        FLegacyImportAnalysis& OutAnalysis,
        FString& OutError)
    {
        OutAnalysis.OriginalSourcePhysicalPath = FPaths::ConvertRelativePathToFull(SelectedFolder);
        FPaths::NormalizeDirectoryName(OutAnalysis.OriginalSourcePhysicalPath);
        OutAnalysis.ModName = FPaths::GetCleanFilename(OutAnalysis.OriginalSourcePhysicalPath);

        if (OutAnalysis.ModName.IsEmpty())
        {
            OutError = TEXT("Could not determine a mod name from the selected folder.");
            return false;
        }

        FText PluginNameFailure;
        if (!FPluginUtils::IsValidPluginName(OutAnalysis.ModName, &PluginNameFailure, nullptr))
        {
            OutError = FString::Printf(
                TEXT("'%s' is not a valid Unreal plugin/mod name:\n%s"),
                *OutAnalysis.ModName,
                *PluginNameFailure.ToString());
            return false;
        }

        if (IPluginManager::Get().FindPlugin(OutAnalysis.ModName).IsValid())
        {
            OutError = FString::Printf(
                TEXT("A plugin or mod named '%s' already exists. Delete/rename it before importing this legacy mod."),
                *OutAnalysis.ModName);
            return false;
        }

        const FString TargetModDirectory = FPaths::Combine(
            FPaths::ProjectDir(),
            TEXT("Mods"),
            OutAnalysis.ModName);

        if (IFileManager::Get().DirectoryExists(*TargetModDirectory))
        {
            OutError = FString::Printf(
                TEXT("The target mod directory already exists:\n%s"),
                *TargetModDirectory);
            return false;
        }

        if (!PrepareLegacySource(SelectedFolder, OutAnalysis, OutError))
        {
            return false;
        }

        // ModMeta is optional. Legacy mods stored their metadata instances
        // globally under /Game/Mods/ModMeta instead of inside each mod folder.
        PrepareLegacyModMetaSource(OutAnalysis);

        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        TArray<FString> PathsToScan;
        PathsToScan.Add(OutAnalysis.SourceVirtualPath);
        AssetRegistry.ScanPathsSynchronous(PathsToScan, true);

        FARFilter SourceFilter;
        SourceFilter.PackagePaths.Add(FName(*OutAnalysis.SourceVirtualPath));
        SourceFilter.bRecursivePaths = true;

        AssetRegistry.GetAssets(SourceFilter, OutAnalysis.Assets);

        OutAnalysis.Assets.RemoveAll(
            [](const FAssetData& AssetData)
            {
                return AssetData.AssetClassPath == UObjectRedirector::StaticClass()->GetClassPathName();
            });

        if (OutAnalysis.Assets.Num() == 0)
        {
            OutError = FString::Printf(
                TEXT("No uncooked Unreal assets were found under %s.\n\nThis importer is intended for the original editor .uasset/.umap files, not cooked files extracted from a .pak."),
                *OutAnalysis.SourceVirtualPath);
            CleanupStagedLegacyCopy(OutAnalysis);
            return false;
        }

        AnalyzeExternalGameDependencies(
            OutAnalysis,
            AssetRegistry);

        TMap<FString, FString> PDMLClasses;
        TMap<FString, FString> PDMLAssets;
        TMap<FString, FString> PDMLGeneratedClassesByAsset;
        BuildPDMLTargetMaps(
            AssetRegistry,
            PDMLClasses,
            PDMLAssets,
            PDMLGeneratedClassesByAsset);

        FindBestLegacyModMetaAsset(
            OutAnalysis,
            AssetRegistry);

        AnalyzeLegacyModMetaCandidateRedirects(
            OutAnalysis,
            AssetRegistry,
            PDMLClasses,
            PDMLAssets,
            PDMLGeneratedClassesByAsset);

        TSet<FString> UnknownParents;

        for (const FAssetData& AssetData : OutAnalysis.Assets)
        {
            FString ParentClassTag;
            const bool bHasParent =
                AssetData.GetTagValue(FName(TEXT("ParentClass")), ParentClassTag) ||
                AssetData.GetTagValue(FName(TEXT("NativeParentClass")), ParentClassTag);

            if (bHasParent)
            {
                ++OutAnalysis.BlueprintCount;

                const FString OldParent = StripExportTextPath(ParentClassTag);
                const FString NewParent = ResolvePDMLClassTarget(OldParent, PDMLClasses);

                if (!NewParent.IsEmpty() &&
                    !OldParent.Equals(NewParent, ESearchCase::IgnoreCase))
                {
                    OutAnalysis.ClassRedirects.Add(OldParent, NewParent);
                    ++OutAnalysis.KnownParentRepairs;
                }
                else if (IsLikelyLegacyModLoaderPath(OldParent) &&
                         !DoesClassPackageExistWithoutLoading(OldParent))
                {
                    UnknownParents.Add(OldParent);
                }
            }

            const FString AssetClassPath = AssetData.AssetClassPath.ToString();
            if (IsLikelyLegacyModLoaderPath(AssetClassPath))
            {
                const FString NewClass = ResolvePDMLClassTarget(AssetClassPath, PDMLClasses);
                if (!NewClass.IsEmpty())
                {
                    OutAnalysis.ClassRedirects.Add(AssetClassPath, NewClass);
                }
            }

            TArray<FName> Dependencies;
            AssetRegistry.GetDependencies(
                AssetData.PackageName,
                Dependencies,
                UE::AssetRegistry::EDependencyCategory::Package);

            for (const FName DependencyName : Dependencies)
            {
                const FString DependencyPackage = DependencyName.ToString();

                if (!IsLikelyLegacyModLoaderPath(DependencyPackage) ||
                    DependencyPackage.StartsWith(
                        OutAnalysis.SourceVirtualPath,
                        ESearchCase::IgnoreCase) ||
                    FPackageName::DoesPackageExist(DependencyPackage))
                {
                    continue;
                }

                // Only remap dependencies whose legacy package is actually
                // missing. Existing inter-mod dependencies must not be silently
                // redirected just because they share a filename with a PDML asset.
                const FString PDMLAssetTarget = ResolvePDMLAssetTarget(
                    DependencyPackage,
                    PDMLAssets);

                if (!PDMLAssetTarget.IsEmpty())
                {
                    const FString OldAssetName = GetPackageLeafName(DependencyPackage);
                    const FSoftObjectPath OldAssetPath(
                        DependencyPackage + TEXT(".") + OldAssetName);
                    const FSoftObjectPath NewAssetPath(PDMLAssetTarget);

                    OutAnalysis.AssetRedirects.Add(OldAssetPath, NewAssetPath);

                    const FString NormalizedAssetName = NormalizeLegacyName(OldAssetName);
                    if (const FString* NewGeneratedClass =
                            PDMLGeneratedClassesByAsset.Find(NormalizedAssetName))
                    {
                        const FString OldGeneratedClass =
                            DependencyPackage + TEXT(".") + OldAssetName + TEXT("_C");
                        OutAnalysis.ClassRedirects.Add(
                            OldGeneratedClass,
                            *NewGeneratedClass);
                    }
                }
            }
        }

        OutAnalysis.UnknownMissingParents = UnknownParents.Num();
        for (const FString& Parent : UnknownParents)
        {
            OutAnalysis.UnknownParentLines.Add(Parent);
        }
        OutAnalysis.UnknownParentLines.Sort();

        return true;
    }

    static void InstallLegacyRedirects(const FLegacyImportAnalysis& Analysis)
    {
        TArray<FCoreRedirect> ClassRedirectList;
        ClassRedirectList.Reserve(Analysis.ClassRedirects.Num());

        for (const TPair<FString, FString>& Pair : Analysis.ClassRedirects)
        {
            ClassRedirectList.Emplace(
                ECoreRedirectFlags::Type_Class,
                Pair.Key,
                Pair.Value);

            UE_LOG(
                LogCrimeForge,
                Display,
                TEXT("Legacy importer class redirect: %s -> %s"),
                *Pair.Key,
                *Pair.Value);
        }

        if (ClassRedirectList.Num() > 0)
        {
            FCoreRedirects::AddRedirectList(
                ClassRedirectList,
                TEXT("CrimeForgeLegacyModImporter"));
        }

        if (Analysis.AssetRedirects.Num() > 0)
        {
            FCoreRedirects::AddAssetRedirects(Analysis.AssetRedirects);

            for (const TPair<FSoftObjectPath, FSoftObjectPath>& Pair : Analysis.AssetRedirects)
            {
                UE_LOG(
                    LogCrimeForge,
                    Display,
                    TEXT("Legacy importer asset redirect: %s -> %s"),
                    *Pair.Key.ToString(),
                    *Pair.Value.ToString());
            }
        }
    }

    static bool CreateBlankCrimeForgeMod(
        const FString& ModName,
        TSharedPtr<IPlugin>& OutPlugin,
        FString& OutError)
    {
        const TSharedPtr<IPlugin> CrimeForgePlugin =
            IPluginManager::Get().FindPlugin(TEXT("CrimeForge"));

        if (!CrimeForgePlugin.IsValid())
        {
            OutError = TEXT("Could not find the CrimeForge plugin while creating the migrated mod.");
            return false;
        }

        const FString BlankTemplateDirectory = FPaths::Combine(
            CrimeForgePlugin->GetBaseDir(),
            TEXT("Templates"),
            TEXT("Blank"));

        if (!IFileManager::Get().DirectoryExists(*BlankTemplateDirectory))
        {
            OutError = FString::Printf(
                TEXT("CrimeForge's Blank template folder does not exist:\n%s"),
                *BlankTemplateDirectory);
            return false;
        }

        FPluginDescriptor Descriptor;
        Descriptor.FriendlyName = ModName;
        Descriptor.Description = TEXT("Migrated from a legacy UE4 ModLoader mod by CrimeForge.");
        Descriptor.Category = TEXT("Mods");
        Descriptor.Version = 1;
        Descriptor.VersionName = TEXT("1.0.0");
        Descriptor.bCanContainContent = true;
        Descriptor.EnabledByDefault = EPluginEnabledByDefault::Enabled;
        Descriptor.AdditionalFieldsToWrite.Add(
            TEXT("SemVersion"),
            MakeShared<FJsonValueString>(TEXT("1.0.0")));

        FString PDMLVersion;
        if (const TSharedPtr<IPlugin> PDML = IPluginManager::Get().FindPlugin(TEXT("PDML"));
            PDML.IsValid())
        {
            PDMLVersion = PDML->GetDescriptor().VersionName;
        }

        FPluginReferenceDescriptor PDMLDependency(TEXT("PDML"), true);
        PDMLDependency.bOptional = false;
        PDMLDependency.AdditionalFieldsToWrite.Add(
            TEXT("SemVersion"),
            MakeShared<FJsonValueString>(PDMLVersion));
        Descriptor.Plugins.Add(PDMLDependency);

        FPluginUtils::FNewPluginParamsWithDescriptor CreationParams;
        CreationParams.Descriptor = Descriptor;
        CreationParams.TemplateFolders.Add(BlankTemplateDirectory);

        FText LoadFailure;
        FPluginUtils::FLoadPluginParams LoadParams;
        LoadParams.OutFailReason = &LoadFailure;
        LoadParams.bEnablePluginInProject = true;
        LoadParams.bSynchronousAssetsScan = true;
        LoadParams.bSelectInContentBrowser = false;
        LoadParams.bUpdateProjectPluginSearchPath = true;

        const FString ModsDirectory = FPaths::Combine(
            FPaths::ProjectDir(),
            TEXT("Mods"));

        /*
         * <Project>/Mods is not one of Unreal's built-in plugin roots
         * (<Project>/Plugins is). Updating the .uproject search path is useful
         * for future sessions, but it does not guarantee that the currently
         * running IPluginManager has already added that directory to its live
         * discovery roots.
         */
        IPluginManager::Get().AddPluginSearchPath(
            ModsDirectory,
            false);

        OutPlugin = FPluginUtils::CreateAndLoadNewPlugin(
            ModName,
            ModsDirectory,
            CreationParams,
            LoadParams);

        if (!OutPlugin.IsValid())
        {
            OutError = LoadFailure.IsEmpty()
                ? FString::Printf(TEXT("Failed to create/load mod '%s'."), *ModName)
                : LoadFailure.ToString();
            return false;
        }

        /*
         * CreateAndLoadNewPlugin() returned a valid IPlugin, but the regular
         * discovery/enabled arrays can still lag behind for a custom external
         * root such as /Mods. Register this exact descriptor immediately.
         */
        FText AddPluginFailure;
        if (!IPluginManager::Get().AddToPluginsList(
                OutPlugin->GetDescriptorFileName(),
                &AddPluginFailure))
        {
            UE_LOG(
                LogCrimeForge,
                Warning,
                TEXT(
                    "Legacy importer created '%s' but could not explicitly add "
                    "its descriptor to IPluginManager: %s"
                ),
                *ModName,
                *AddPluginFailure.ToString());
        }

        IPluginManager::Get().RefreshPluginsList();

        FModWizardDefinition Wizard;
        Wizard.RenameTemplateAssetsForMods(ModName);

        // Keep the same Asset Manager registration path used by normal CrimeForge
        // mod creation/packaging.
        FCrimeForgeModule& CrimeForgeModule =
            FModuleManager::LoadModuleChecked<FCrimeForgeModule>(TEXT("CrimeForge"));
        CrimeForgeModule.EnsureAssetManagerRegistrationForPlugin(*OutPlugin);

        return true;
    }

    static FString BuildAnalysisSummary(const FLegacyImportAnalysis& Analysis)
    {
        FString Summary = FString::Printf(
            TEXT(
                "Import legacy UE4 mod\n\n"
                "Selected source:\n%s\n\n"
                "Working source:\n%s\n\n"
                "Source mount:\n%s\n\n"
                "New CrimeForge mod:\n/%s\n\n"
                "Assets found: %d\n"
                "Blueprint-like assets: %d\n"
                "Legacy parent/class redirects found: %d\n"
                "Legacy asset redirects found: %d\n"
                "Unknown missing parents: %d\n\n"
                "The source assets will be moved into /%s while preserving their relative folder structure. "
                "Known legacy ModLoader parent classes will be redirected/reparented to matching PDML classes before the assets are resaved.\n\n"
                "Continue?"),
            *Analysis.OriginalSourcePhysicalPath,
            Analysis.bStagedExternalCopy ? *Analysis.SourcePhysicalPath : TEXT("same as selected source"),
            *Analysis.SourceVirtualPath,
            *Analysis.ModName,
            Analysis.Assets.Num(),
            Analysis.BlueprintCount,
            Analysis.ClassRedirects.Num(),
            Analysis.AssetRedirects.Num(),
            Analysis.UnknownMissingParents,
            *Analysis.ModName);

        if (Analysis.UnknownParentLines.Num() > 0)
        {
            Summary += TEXT("\n\nUnknown parents (these assets may fail to load and will be left in the legacy folder):\n");
            const int32 MaxLines = FMath::Min(Analysis.UnknownParentLines.Num(), 8);
            for (int32 Index = 0; Index < MaxLines; ++Index)
            {
                Summary += TEXT("  ") + Analysis.UnknownParentLines[Index] + TEXT("\n");
            }
            if (Analysis.UnknownParentLines.Num() > MaxLines)
            {
                Summary += FString::Printf(
                    TEXT("  ...and %d more\n"),
                    Analysis.UnknownParentLines.Num() - MaxLines);
            }
        }

        Summary += FString::Printf(
            TEXT(
                "\n\nExternal /Game dependencies:\n"
                "  Will migrate from old project: %d\n"
                "  Already in PAYDAY3 (reuse): %d\n"
                "  Missing from both projects: %d"),
            Analysis.ExternalGameDependenciesToMigrate.Num(),
            Analysis.ExternalGameDependenciesReused.Num(),
            Analysis.ExternalGameDependenciesMissingFromSource.Num());

        auto AppendDependencyPreview =
            [&Summary](
                const TCHAR* Heading,
                const TArray<FString>& Packages)
            {
                if (Packages.Num() == 0)
                {
                    return;
                }

                Summary += TEXT("\n") + FString(Heading);

                const int32 MaxLines =
                    FMath::Min(
                        Packages.Num(),
                        6);

                for (int32 Index = 0;
                     Index < MaxLines;
                     ++Index)
                {
                    Summary += TEXT("\n  -> ") +
                        Packages[Index];
                }

                if (Packages.Num() > MaxLines)
                {
                    Summary += FString::Printf(
                        TEXT("\n  ...and %d more"),
                        Packages.Num() - MaxLines);
                }
            };

        AppendDependencyPreview(
            TEXT("Will migrate in-place under /Game:"),
            Analysis.ExternalGameDependenciesToMigrate);

        AppendDependencyPreview(
            TEXT("Will reuse existing PAYDAY3 assets:"),
            Analysis.ExternalGameDependenciesReused);

        AppendDependencyPreview(
            TEXT("Still unresolved:"),
            Analysis.ExternalGameDependenciesMissingFromSource);

        if (!Analysis.LegacyModMetaObjectPath.IsEmpty())
        {
            Summary += FString::Printf(
                TEXT(
                    "\n\nLegacy ModMeta match:\n%s\n"
                    "References into this mod: %d"),
                *Analysis.LegacyModMetaObjectPath,
                Analysis.LegacyModMetaReferenceScore);

            const int32 MaxReferences =
                FMath::Min(
                    Analysis.LegacyModMetaMatchedReferences.Num(),
                    5);

            for (int32 Index = 0;
                 Index < MaxReferences;
                 ++Index)
            {
                Summary += TEXT("\n  -> ") +
                    Analysis.LegacyModMetaMatchedReferences[Index];
            }

            if (Analysis.LegacyModMetaMatchedReferences.Num() >
                MaxReferences)
            {
                Summary += FString::Printf(
                    TEXT("\n  ...and %d more"),
                    Analysis.LegacyModMetaMatchedReferences.Num() -
                    MaxReferences);
            }
        }
        else
        {
            Summary +=
                TEXT(
                    "\n\nLegacy ModMeta match:\n"
                    "No /Game/Mods/ModMeta asset referenced this mod.");
        }

        return Summary;
    }

    static bool ExecuteMigration(
        const FLegacyImportAnalysis& Analysis,
        FString& OutReport)
    {
        InstallLegacyRedirects(Analysis);

        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
                TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        /*
         * IMPORTANT: do not load the legacy ModMeta first. A ModMeta can hold a
         * reference to the ModActor and cause that Blueprint to load
         * transitively. All missing reconstructed /Game packages must exist,
         * load, compile and save before that is allowed to happen.
         */
        TArray<FString> MigratedExternalGamePackages;
        TArray<FString> ReusedExternalGamePackages;
        TArray<FString> ExternalGameDependencyFailures;

        MigrateExternalGameDependencies(
            Analysis,
            AssetRegistry,
            MigratedExternalGamePackages,
            ReusedExternalGamePackages,
            ExternalGameDependencyFailures);

        TArray<FString> PreparedExternalGamePackages;
        TArray<FString> ExternalGamePreparationFailures;

        PrepareMigratedExternalGameDependencies(
            AssetRegistry,
            MigratedExternalGamePackages,
            PreparedExternalGamePackages,
            ExternalGamePreparationFailures);

        UObject* LegacyMetadataObject = nullptr;

        if (Analysis.LegacyModMetaAsset.IsValid())
        {
            LegacyMetadataObject =
                Analysis.LegacyModMetaAsset.GetAsset();

            if (!LegacyMetadataObject)
            {
                UE_LOG(
                    LogCrimeForge,
                    Warning,
                    TEXT(
                        "Matched legacy ModMeta could not be loaded: %s"
                    ),
                    *Analysis.LegacyModMetaObjectPath);
            }
        }

        TSharedPtr<IPlugin> NewPlugin;
        FString CreateError;
        if (!CreateBlankCrimeForgeMod(
                Analysis.ModName,
                NewPlugin,
                CreateError))
        {
            OutReport =
                TEXT(
                    "Legacy import failed while creating the new CrimeForge "
                    "mod:\n\n") +
                CreateError;
            return false;
        }

        UEditorAssetSubsystem* EditorAssetSubsystem =
            GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;

        if (!EditorAssetSubsystem)
        {
            OutReport = TEXT("The new mod was created, but the Editor Asset Subsystem was unavailable. No legacy assets were moved.");
            return false;
        }

        TArray<FLegacyMovePlan> Plans;
        TArray<FString> FailedToLoad;
        TArray<FString> Collisions;

        for (const FAssetData& AssetData : Analysis.Assets)
        {
            UObject* Asset = AssetData.GetAsset();
            if (!Asset)
            {
                FailedToLoad.Add(AssetData.GetObjectPathString());
                continue;
            }

            FString SourcePackagePath = AssetData.PackagePath.ToString();
            if (!SourcePackagePath.StartsWith(
                    Analysis.SourceVirtualPath,
                    ESearchCase::IgnoreCase))
            {
                continue;
            }

            FString RelativeFolder = SourcePackagePath.Mid(Analysis.SourceVirtualPath.Len());
            if (!RelativeFolder.IsEmpty() && !RelativeFolder.StartsWith(TEXT("/")))
            {
                RelativeFolder = TEXT("/") + RelativeFolder;
            }

            const FString TargetPackagePath =
                TEXT("/") + Analysis.ModName + RelativeFolder;

            const FString AssetName = AssetData.AssetName.ToString();
            const FString TargetObjectPath =
                TargetPackagePath + TEXT("/") + AssetName + TEXT(".") + AssetName;

            if (UEditorAssetLibrary::DoesAssetExist(TargetObjectPath))
            {
                Collisions.Add(
                    FString::Printf(
                        TEXT("%s -> %s"),
                        *AssetData.GetObjectPathString(),
                        *TargetObjectPath));
                continue;
            }

            FLegacyMovePlan& Plan = Plans.AddDefaulted_GetRef();
            Plan.AssetData = AssetData;
            Plan.Asset = Asset;
            Plan.TargetPackagePath = TargetPackagePath;
            Plan.TargetObjectPath = TargetObjectPath;

            FString ParentClassTag;
            if (AssetData.GetTagValue(FName(TEXT("ParentClass")), ParentClassTag) ||
                AssetData.GetTagValue(FName(TEXT("NativeParentClass")), ParentClassTag))
            {
                Plan.OriginalParentClassPath = StripExportTextPath(ParentClassTag);
                if (const FString* NewParent = Analysis.ClassRedirects.Find(Plan.OriginalParentClassPath))
                {
                    Plan.ReplacementParentClassPath = *NewParent;
                }
            }
        }

        TArray<FAssetRenameData> RenameData;
        RenameData.Reserve(Plans.Num());
        for (const FLegacyMovePlan& Plan : Plans)
        {
            RenameData.Emplace(
                Plan.Asset,
                Plan.TargetPackagePath,
                Plan.AssetData.AssetName.ToString());
        }

        bool bRenameCallSucceeded = true;
        if (RenameData.Num() > 0)
        {
            FAssetToolsModule& AssetToolsModule =
                FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
            bRenameCallSucceeded = AssetToolsModule.Get().RenameAssets(RenameData);
        }

        int32 MovedCount = 0;
        int32 ReparentedCount = 0;
        int32 BlueprintCompileErrors = 0;
        TArray<FString> MoveFailures;
        TArray<UObject*> AssetsToSave;

        for (FLegacyMovePlan& Plan : Plans)
        {
            UObject* Asset = Plan.Asset;
            if (!Asset)
            {
                MoveFailures.Add(Plan.AssetData.GetObjectPathString());
                continue;
            }

            const FString CurrentPackageName = Asset->GetOutermost()->GetName();
            if (!CurrentPackageName.StartsWith(
                    TEXT("/") + Analysis.ModName,
                    ESearchCase::IgnoreCase))
            {
                MoveFailures.Add(Plan.AssetData.GetObjectPathString());
                continue;
            }

            ++MovedCount;

            if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
            {
                if (!Plan.ReplacementParentClassPath.IsEmpty())
                {
                    UClass* NewParent = LoadObject<UClass>(
                        nullptr,
                        *Plan.ReplacementParentClassPath);

                    if (NewParent)
                    {
                        /*
                         * UE 5.5 does not expose
                         * FBlueprintEditorUtils::ReparentBlueprint().
                         * Update ParentClass directly, then force a structural
                         * Blueprint refresh before compiling.
                         */
                        if (Blueprint->ParentClass != NewParent)
                        {
                            Blueprint->Modify();
                            Blueprint->ParentClass = NewParent;

                            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(
                                Blueprint);

                            FBlueprintEditorUtils::RefreshAllNodes(
                                Blueprint);
                        }

                        ++ReparentedCount;
                    }
                    else
                    {
                        MoveFailures.Add(
                            FString::Printf(
                                TEXT("%s (could not load replacement parent %s)"),
                                *Blueprint->GetPathName(),
                                *Plan.ReplacementParentClassPath));
                    }
                }

                FKismetEditorUtilities::CompileBlueprint(Blueprint);
                if (Blueprint->Status == BS_Error)
                {
                    ++BlueprintCompileErrors;
                }
            }

            Asset->MarkPackageDirty();
            AssetsToSave.AddUnique(Asset);
        }

        if (AssetsToSave.Num() > 0)
        {
            EditorAssetSubsystem->SaveLoadedAssets(AssetsToSave, false);
        }

        const FString TargetRoot = TEXT("/") + Analysis.ModName;
        EditorAssetSubsystem->SaveDirectory(TargetRoot, false, true);

        // Rebuild the generated DA now that legacy ModActor Blueprints have
        // their final PDML parent and final /<ModName> paths.
        FModWizardDefinition Wizard;
        const bool bMetadataPopulated =
            Wizard.PopulateGeneratedModDataAssetForPlugin(Analysis.ModName);

        int32 LegacyMetadataPropertiesCopied = 0;
        TArray<FString> LegacyMetadataCopiedProperties;
        TArray<FString> LegacyMetadataSkippedProperties;

        if (LegacyMetadataObject)
        {
            const FString GeneratedMetadataObjectPath =
                FString::Printf(
                    TEXT("/%s/DA_%s.DA_%s"),
                    *Analysis.ModName,
                    *Analysis.ModName,
                    *Analysis.ModName);

            UObject* GeneratedMetadataObject =
                LoadObject<UObject>(
                    nullptr,
                    *GeneratedMetadataObjectPath);

            if (GeneratedMetadataObject)
            {
                LegacyMetadataPropertiesCopied =
                    CopyCompatibleLegacyMetadataProperties(
                        LegacyMetadataObject,
                        GeneratedMetadataObject,
                        LegacyMetadataCopiedProperties,
                        LegacyMetadataSkippedProperties);

                if (LegacyMetadataPropertiesCopied > 0)
                {
                    TArray<UObject*> MetadataToSave;
                    MetadataToSave.Add(
                        GeneratedMetadataObject);

                    EditorAssetSubsystem->SaveLoadedAssets(
                        MetadataToSave,
                        false);
                }

                UE_LOG(
                    LogCrimeForge,
                    Display,
                    TEXT(
                        "Legacy ModMeta migration for '%s': copied %d "
                        "compatible property value(s) from %s"
                    ),
                    *Analysis.ModName,
                    LegacyMetadataPropertiesCopied,
                    *Analysis.LegacyModMetaObjectPath);
            }
        }

        TArray<FString> SourceRescan;
        SourceRescan.Add(Analysis.SourceVirtualPath);
        AssetRegistry.ScanPathsSynchronous(SourceRescan, true);

        FARFilter RemainingFilter;
        RemainingFilter.PackagePaths.Add(FName(*Analysis.SourceVirtualPath));
        RemainingFilter.bRecursivePaths = true;

        TArray<FAssetData> RemainingAssets;
        AssetRegistry.GetAssets(RemainingFilter, RemainingAssets);

        int32 RemainingNonRedirectors = 0;

        for (const FAssetData& AssetData : RemainingAssets)
        {
            if (AssetData.AssetClassPath !=
                UObjectRedirector::StaticClass()->GetClassPathName())
            {
                ++RemainingNonRedirectors;
            }
        }

        /*
         * Never delete the old virtual directory through
         * UEditorAssetSubsystem::DeleteDirectory() in the same operation that
         * renamed these packages. UE 5.5 crashed here while force-deleting the
         * newly-created redirector packages.
         *
         * External imports use scratch staging, so that scratch mount can be
         * safely unregistered and its physical files removed. If the user
         * deliberately imports a folder already inside this project's Content,
         * leave the source redirectors in place for manual fix-up/cleanup.
         */
        const bool bTemporaryStagingRemoved =
            CleanupStagedLegacyCopy(Analysis);

        OutReport = FString::Printf(
            TEXT(
                "Legacy migration finished: %s\n\n"
                "Source: %s\n"
                "Target: /%s\n\n"
                "Assets discovered: %d\n"
                "Assets moved: %d\n"
                "Class redirects installed: %d\n"
                "Asset redirects installed: %d\n"
                "Blueprints reparented: %d\n"
                "Blueprint compile errors: %d\n"
                "Generated PDA_ModMeta populated: %s\n"
                "Legacy ModMeta matched: %s\n"
                "Legacy ModMeta references into mod: %d\n"
                "Legacy metadata properties copied: %d\n"
                "External /Game assets migrated in-place: %d\n"
                "External /Game assets prepared before ModActor load: %d\n"
                "External /Game assets reused: %d\n"
                "External /Game dependency failures: %d\n"
                "External /Game preparation failures: %d\n"
                "Temporary external staging removed: %s\n"
                "In-project source redirectors retained: %s\n"
                "Original external source preserved: %s\n"
                "AssetTools rename result: %s\n\n"
                "Failed to load: %d\n"
                "Target collisions: %d\n"
                "Move/repair failures: %d\n"
                "Unknown missing parents detected before import: %d"),
            (FailedToLoad.Num() == 0 && MoveFailures.Num() == 0 && Collisions.Num() == 0)
                ? TEXT("SUCCESS")
                : TEXT("COMPLETED WITH WARNINGS"),
            *Analysis.SourceVirtualPath,
            *Analysis.ModName,
            Analysis.Assets.Num(),
            MovedCount,
            Analysis.ClassRedirects.Num(),
            Analysis.AssetRedirects.Num(),
            ReparentedCount,
            BlueprintCompileErrors,
            bMetadataPopulated ? TEXT("Yes") : TEXT("No"),
            Analysis.LegacyModMetaObjectPath.IsEmpty()
                ? TEXT("No")
                : *Analysis.LegacyModMetaObjectPath,
            Analysis.LegacyModMetaReferenceScore,
            LegacyMetadataPropertiesCopied,
            MigratedExternalGamePackages.Num(),
            PreparedExternalGamePackages.Num(),
            ReusedExternalGamePackages.Num(),
            ExternalGameDependencyFailures.Num(),
            ExternalGamePreparationFailures.Num(),
            Analysis.bStagedExternalCopy
                ? (bTemporaryStagingRemoved ? TEXT("Yes") : TEXT("No"))
                : TEXT("N/A"),
            Analysis.bStagedExternalCopy ? TEXT("No") : TEXT("Yes"),
            Analysis.bStagedExternalCopy ? TEXT("Yes") : TEXT("N/A - source was already in this project"),
            bRenameCallSucceeded ? TEXT("Success") : TEXT("Reported one or more failures"),
            FailedToLoad.Num(),
            Collisions.Num(),
            MoveFailures.Num(),
            Analysis.UnknownMissingParents);

        auto AppendExamples = [&OutReport](const TCHAR* Heading, const TArray<FString>& Values)
        {
            if (Values.Num() == 0)
            {
                return;
            }

            OutReport += FString::Printf(TEXT("\n\n%s:\n"), Heading);
            const int32 MaxItems = FMath::Min(Values.Num(), 12);
            for (int32 Index = 0; Index < MaxItems; ++Index)
            {
                OutReport += TEXT("  ") + Values[Index] + TEXT("\n");
            }
            if (Values.Num() > MaxItems)
            {
                OutReport += FString::Printf(
                    TEXT("  ...and %d more\n"),
                    Values.Num() - MaxItems);
            }
        };

        AppendExamples(TEXT("Failed to load"), FailedToLoad);
        AppendExamples(TEXT("Target collisions"), Collisions);
        AppendExamples(TEXT("Move/repair failures"), MoveFailures);
        AppendExamples(TEXT("Unknown legacy parents"), Analysis.UnknownParentLines);
        AppendExamples(
            TEXT("Legacy metadata properties copied"),
            LegacyMetadataCopiedProperties);
        AppendExamples(
            TEXT("Legacy metadata properties skipped (type changed)"),
            LegacyMetadataSkippedProperties);
        AppendExamples(
            TEXT("External /Game assets migrated in-place"),
            MigratedExternalGamePackages);
        AppendExamples(
            TEXT("External /Game assets reused from PAYDAY3"),
            ReusedExternalGamePackages);
        AppendExamples(
            TEXT("External /Game dependency failures"),
            ExternalGameDependencyFailures);
        AppendExamples(
            TEXT("External /Game assets prepared before ModActor load"),
            PreparedExternalGamePackages);
        AppendExamples(
            TEXT("External /Game preparation failures"),
            ExternalGamePreparationFailures);

        return MovedCount > 0;
    }
}

bool FLegacyModImporter::ImportFromFolderDialog(
    FString& OutImportedModName,
    FString& OutReport)
{
    OutImportedModName.Reset();
    OutReport.Reset();

    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (!DesktopPlatform)
    {
        OutReport = TEXT("DesktopPlatform is unavailable; CrimeForge could not open the legacy mod folder picker.");
        return false;
    }

    const void* ParentWindowHandle =
        FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);

    FString SelectedFolder;
    FString InitialFolder = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(
            FPaths::ProjectContentDir(),
            TEXT("Content"),
            TEXT("Mods")));

    if (!IFileManager::Get().DirectoryExists(*InitialFolder))
    {
        InitialFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    }

    if (!DesktopPlatform->OpenDirectoryDialog(
            ParentWindowHandle,
            TEXT("Select Legacy UE4 Mod Folder"),
            InitialFolder,
            SelectedFolder))
    {
        return false;
    }

    FLegacyImportAnalysis Analysis;
    FString AnalysisError;
    if (!AnalyzeLegacyFolder(SelectedFolder, Analysis, AnalysisError))
    {
        OutReport = AnalysisError;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(OutReport));
        return false;
    }

    const EAppReturnType::Type Confirmation = FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::FromString(BuildAnalysisSummary(Analysis)));

    if (Confirmation != EAppReturnType::Yes)
    {
        CleanupStagedLegacyCopy(Analysis);
        return false;
    }

    const bool bImported = ExecuteMigration(Analysis, OutReport);
    if (!bImported)
    {
        CleanupStagedLegacyCopy(Analysis);
    }
    OutImportedModName = bImported ? Analysis.ModName : FString();

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(OutReport));
    return bImported;
}
