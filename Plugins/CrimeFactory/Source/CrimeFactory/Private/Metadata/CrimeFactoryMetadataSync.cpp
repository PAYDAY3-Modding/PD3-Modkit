#include "Metadata/CrimeFactoryMetadataSync.h"

#include "Editor/CrimeFactory.h"
#include "Metadata/ModMetadataObject.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/TransactionObjectEvent.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace
{
    // CrimeFactoryMetadataFieldKeys deliberately match the PDA_ModMeta variables, including Blueprint variable GUID suffixes.
    const TCHAR* CrimeFactoryMetadataFieldKeys[] = {TEXT("ModID"), TEXT("ModName"), TEXT("ModVersion"), TEXT("ModDescription"),
        TEXT("ModAuthors"), TEXT("ModPageURL"), TEXT("ModEnvironment"), TEXT("ModLoaderVersion"), TEXT("Dependencies")};
    struct FState
    {
        TWeakObjectPtr<UObject> Asset;
        TSharedPtr<FJsonObject> AssetSnapshot;
        FString DiskText;
    };
    TMap<FString, FState> States;
    TSet<FString> Pending;
    FDelegateHandle MetadataPropertyChangedHandle, TransactionHandle, LoadHandle, PluginHandle, SaveHandle;
    FTSTicker::FDelegateHandle TickHandle;
    bool bSynchronizing = false;
    TSharedPtr<IPlugin> PluginForAsset(UObject* Asset);

    bool IsProjectMod(const IPlugin& Plugin)
    {
        return FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Plugin.GetBaseDir()),
            FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Mods")));
    }

    bool IsBaseDependency(const FPluginReferenceDescriptor& Reference)
    {
        auto Json = MakeShared<FJsonObject>();
        if (Reference.CachedJson) FJsonObject::Duplicate(Reference.CachedJson, Json);
        Reference.UpdateJson(*Json);
        bool bBase = false;
        Json->TryGetBoolField(TEXT("BasePlugin"), bBase);
        return bBase || Reference.Name.Equals(TEXT("PDML"), ESearchCase::IgnoreCase);
    }

    FString MetadataPath(const IPlugin& Plugin)
    {
        const FString Name = TEXT("DA_") + Plugin.GetName();
        return (Plugin.GetMountedAssetPath() / Name) + TEXT(".") + Name;
    }

    FString Normalize(FString Name)
    {
        FString Result;
        for (const TCHAR C : Name) if (FChar::IsAlnum(C)) Result.AppendChar(FChar::ToLower(C));
        return Result;
    }

    FProperty* FindField(UObject& Asset, const FString& Key)
    {
        const FString Normalized = Normalize(Key);
        for (TFieldIterator<FProperty> It(Asset.GetClass()); It; ++It)
        {
            FString Name = It->GetName();
            int32 Suffix;
            // User-defined Blueprint properties can include _<number>_<GUID>.
            if (Name.FindChar(TEXT('_'), Suffix)) Name.LeftInline(Suffix);
            if (Normalize(Name) == Normalized || Normalize(It->GetDisplayNameText().ToString()) == Normalized) return *It;
        }
        return nullptr;
    }

    UEnum* FieldEnum(FProperty* Field)
    {
        if (const auto* Enum = CastField<FEnumProperty>(Field)) return Enum->GetEnum();
        if (const auto* Byte = CastField<FByteProperty>(Field)) return Byte->Enum;
        return nullptr;
    }

    bool ReadText(FProperty* Field, const void* Ptr, FString& Out)
    {
        if (const auto* Value = CastField<FStrProperty>(Field)) { Out = Value->GetPropertyValue(Ptr); return true; }
        if (const auto* Value = CastField<FNameProperty>(Field)) { Out = Value->GetPropertyValue(Ptr).ToString(); return true; }
        if (const auto* Value = CastField<FTextProperty>(Field)) { Out = Value->GetPropertyValue(Ptr).ToString(); return true; }
        if (const auto* Enum = FieldEnum(Field))
        {
            const int64 Value = CastField<FEnumProperty>(Field)
                ? CastFieldChecked<FEnumProperty>(Field)->GetUnderlyingProperty()->GetSignedIntPropertyValue(Ptr)
                : CastFieldChecked<FByteProperty>(Field)->GetPropertyValue(Ptr);
            Out = Enum->GetDisplayNameTextByValue(Value).ToString();
        }
        else return false;
        return true;
    }

    bool WriteText(FProperty* Field, void* Ptr, const FString& Text)
    {
        if (auto* Value = CastField<FStrProperty>(Field)) { Value->SetPropertyValue(Ptr, Text); return true; }
        if (auto* Value = CastField<FNameProperty>(Field)) { Value->SetPropertyValue(Ptr, Text.IsEmpty() ? NAME_None : FName(*Text)); return true; }
        if (auto* Value = CastField<FTextProperty>(Field)) { Value->SetPropertyValue(Ptr, FText::FromString(Text)); return true; }
        if (const auto* Enum = FieldEnum(Field))
        {
            int64 Value = INDEX_NONE;
            for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
                if (Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(Text, ESearchCase::IgnoreCase)
                    || Enum->GetNameStringByIndex(Index).Equals(Text, ESearchCase::IgnoreCase)) Value = Enum->GetValueByIndex(Index);
            if (Value == INDEX_NONE) return false;
            if (auto* EnumField = CastField<FEnumProperty>(Field)) EnumField->GetUnderlyingProperty()->SetIntPropertyValue(Ptr, Value);
            else CastFieldChecked<FByteProperty>(Field)->SetPropertyValue(Ptr, Value);
        }
        else return false;
        return true;
    }

    TSharedRef<FJsonObject> ReadAsset(UObject& Asset)
    {
        const auto Result = MakeShared<FJsonObject>();
        for (const TCHAR* Key : CrimeFactoryMetadataFieldKeys)
        {
            FProperty* Field = FindField(Asset, Key);
            if (!Field) continue;
            void* Ptr = Field->ContainerPtrToValuePtr<void>(&Asset);
            if (const auto* Array = CastField<FArrayProperty>(Field))
            {
                FScriptArrayHelper Helper(Array, Ptr);
                TArray<TSharedPtr<FJsonValue>> Values;
                if (FCString::Strcmp(Key, TEXT("Dependencies")) == 0)
                {
                    if (const auto* Objects = CastField<FObjectPropertyBase>(Array->Inner))
                    {
                        for (int32 Index = 0; Index < Helper.Num(); ++Index)
                        {
                            FString Path;
                            if (const auto* Soft = CastField<FSoftObjectProperty>(Array->Inner))
                                Path = Soft->GetPropertyValue(Helper.GetRawPtr(Index)).ToSoftObjectPath().ToString();
                            else if (const auto* Object = Objects->GetObjectPropertyValue(Helper.GetRawPtr(Index))) Path = Object->GetPathName();
                            Values.Add(MakeShared<FJsonValueString>(Path));
                        }
                        Result->SetArrayField(Key, Values);
                    }
                    continue;
                }
                bool bSupported = CastField<FStrProperty>(Array->Inner) || CastField<FTextProperty>(Array->Inner) || CastField<FNameProperty>(Array->Inner);
                if (!bSupported) continue; // Struct dependencies are not interchangeable with string IDs.
                for (int32 Index = 0; Index < Helper.Num(); ++Index)
                {
                    FString Value;
                    ReadText(Array->Inner, Helper.GetRawPtr(Index), Value);
                    Values.Add(MakeShared<FJsonValueString>(Value));
                }
                Result->SetArrayField(Key, Values);
            }
            else
            {
                FString Value;
                if (ReadText(Field, Ptr, Value)) Result->SetStringField(Key, Value);
            }
        }
        return Result;
    }

    bool Equal(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
    {
        return A.IsValid() && B.IsValid() && FJsonValue::CompareEqual(*A, *B);
    }

    TSharedRef<FJsonObject> DescriptorJson(const FPluginDescriptor& Descriptor)
    {
        auto Json = MakeShared<FJsonObject>();
        if (Descriptor.CachedJson) FJsonObject::Duplicate(Descriptor.CachedJson, Json);
        Descriptor.UpdateJson(*Json);
        return Json;
    }

    TSharedRef<FJsonObject> ReadDescriptor(IPlugin& Plugin, const FJsonObject& ExistingAsset)
    {
        const auto& Descriptor = Plugin.GetDescriptor();
        const auto Json = DescriptorJson(Descriptor);
        const auto Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("ModID"), Plugin.GetName());
        Result->SetStringField(TEXT("ModName"), Descriptor.FriendlyName.IsEmpty() ? Plugin.GetName() : Descriptor.FriendlyName);
        FString Version = Descriptor.VersionName;
        if (Version.IsEmpty()) Json->TryGetStringField(TEXT("SemVersion"), Version);
        Result->SetStringField(TEXT("ModVersion"), Version);
        Result->SetStringField(TEXT("ModDescription"), Descriptor.Description);
        const TArray<TSharedPtr<FJsonValue>>* Authors;
        TArray<FString> AuthorNames;
        // An explicit author array is independent of the descriptor's CreatedBy credit.
        if (Json->TryGetArrayField(TEXT("ModAuthors"), Authors))
            for (const auto& Author : *Authors) AuthorNames.Add(Author->AsString());
        else if (!Descriptor.CreatedBy.IsEmpty()) AuthorNames.Add(Descriptor.CreatedBy);
        // Preserve legacy multi-author arrays when their visible author string agrees.
        if (!Json->HasField(TEXT("ModAuthors")))
        {
            const TArray<TSharedPtr<FJsonValue>>* ExistingAuthors;
            if (ExistingAsset.TryGetArrayField(TEXT("ModAuthors"), ExistingAuthors))
            {
                TArray<FString> Names;
                for (const auto& Author : *ExistingAuthors) Names.Add(Author->AsString());
                if (FString::Join(Names, TEXT(", ")) == Descriptor.CreatedBy) AuthorNames = MoveTemp(Names);
            }
        }
        TArray<TSharedPtr<FJsonValue>> AuthorValues;
        for (const auto& Author : AuthorNames) AuthorValues.Add(MakeShared<FJsonValueString>(Author));
        Result->SetArrayField(TEXT("ModAuthors"), AuthorValues);
        // Optional fields did not exist in older .uplugins. Preserve their DA values until explicitly edited.
        for (const TCHAR* Key : {TEXT("ModPageURL"), TEXT("ModEnvironment"), TEXT("ModLoaderVersion")})
        {
            if (const auto* Value = Json->Values.Find(Key)) { Result->SetField(Key, *Value); continue; }
            if (const auto* Value = ExistingAsset.Values.Find(Key)) Result->SetField(Key, *Value);
        }
        return Result;
    }

    bool ResolveDependencies(IPlugin& Plugin, UObject& Asset, TArray<TStrongObjectPtr<UObject>>& Objects, FString& Error)
    {
        const auto* Array = CastField<FArrayProperty>(FindField(Asset, TEXT("Dependencies")));
        const auto* Inner = Array ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
        if (!Inner)
        {
            Error = Asset.GetPathName() + TEXT(": Dependencies must be an array of mod metadata DA references.");
            return false;
        }
        TSet<FString> Seen;
        for (const auto& Reference : Plugin.GetDescriptor().Plugins)
        {
            if (!Reference.bEnabled || IsBaseDependency(Reference) || Seen.Contains(Reference.Name)) continue;
            Seen.Add(Reference.Name);
            const auto Dependency = IPluginManager::Get().FindPlugin(Reference.Name);
            if (!Dependency)
            {
                if (Reference.bOptional) continue;
                Error = Plugin.GetName() + TEXT(": dependency plugin '") + Reference.Name + TEXT("' is unavailable. Install it and Refresh Mods, or mark a game/engine dependency as Base Plugin.");
                return false;
            }
            if (!IsProjectMod(*Dependency)) continue; // Engine/game plugins have no PDML mod metadata.
            const FString Path = MetadataPath(*Dependency);
            if (Dependency->GetName() == Plugin.GetName())
            { Error = Plugin.GetName() + TEXT(": a mod cannot depend on itself."); return false; }
            UObject* Object = FSoftObjectPath(Path).ResolveObject();
            if (!Object && FPackageName::DoesPackageExist(FSoftObjectPath(Path).GetLongPackageName()))
                Object = LoadObject<UObject>(nullptr, *Path);
            if (!Object || !Object->IsA(Inner->PropertyClass))
            {
                Error = Plugin.GetName() + TEXT(": dependency '") + Reference.Name + TEXT("' needs a compatible metadata DA at ") + Path
                    + TEXT(". Restore/create that mod's metadata and enable/Refresh Mods before forging.");
                return false;
            }
            Objects.Emplace(Object);
        }
        return true;
    }

    bool WriteDependencyReferences(IPlugin& Plugin, FPluginDescriptor& Descriptor, const FJsonObject& Values, FString& Error)
    {
        const TArray<TSharedPtr<FJsonValue>>* Paths;
        if (!Values.TryGetArrayField(TEXT("Dependencies"), Paths)) return false;
        TArray<FString> Names;
        for (const auto& Value : *Paths)
        {
            const FString Path = Value->AsString();
            const auto* Object = FSoftObjectPath(Path).ResolveObject();
            const auto Dependency = PluginForAsset(const_cast<UObject*>(Object));
            if (!Dependency || Path != MetadataPath(*Dependency))
            {
                Error = Plugin.GetName() + TEXT(": dependency reference '") + Path
                    + TEXT("' is not a generated mod metadata DA. Select /<ModName>/DA_<ModName> in Dependencies.");
                return false;
            }
            if (Dependency->GetName().Equals(TEXT("PDML"), ESearchCase::IgnoreCase)) continue;
            if (Dependency->GetName() == Plugin.GetName())
            { Error = Plugin.GetName() + TEXT(": a mod cannot depend on its own metadata DA."); return false; }
            Names.AddUnique(Dependency->GetName());
        }
        // Preserve PDML, engine/base dependencies and disabled entries. Retained mod
        // references keep optional/version/platform/unknown descriptor fields intact.
        Descriptor.Plugins.RemoveAll([&Names](const FPluginReferenceDescriptor& Reference)
        {
            if (!Reference.bEnabled || IsBaseDependency(Reference) || Names.Contains(Reference.Name)) return false;
            const auto Dependency = IPluginManager::Get().FindPlugin(Reference.Name);
            return Dependency && IsProjectMod(*Dependency);
        });
        for (const auto& Name : Names)
        {
            auto* Existing = Descriptor.Plugins.FindByPredicate([&Name](const auto& Reference) { return Reference.Name == Name; });
            if (Existing)
            {
                if (IsBaseDependency(*Existing))
                { Error = Name + TEXT(" is marked Base Plugin. Clear that flag before using its mod metadata as a dependency."); return false; }
                Existing->bEnabled = true;
            }
            else Descriptor.Plugins.Emplace(Name, true);
        }
        return true;
    }

    bool WriteDescriptor(IPlugin& Plugin, const FJsonObject& Values, const TSet<FString>& Changed, FString& Error)
    {
        FPluginDescriptor Descriptor;
        FText Reason;
        // Read the latest file so unrelated external settings are never replaced by a stale cached descriptor.
        if (!Descriptor.Load(Plugin.GetDescriptorFileName(), Reason)) { Error = Reason.ToString(); return false; }
        auto SetExtra = [&Descriptor, &Values](const FString& Key, const FString& Target)
        { if (const auto* Value = Values.Values.Find(Key)) Descriptor.AdditionalFieldsToWrite.Add(Target, *Value); };
        for (const FString& Key : Changed)
        {
            if (Key == TEXT("Dependencies"))
            {
                if (!WriteDependencyReferences(Plugin, Descriptor, Values, Error)) return false;
                continue;
            }
            FString Text;
            Values.TryGetStringField(Key, Text);
            if (Key == TEXT("ModID")) continue; // Plugin mount identity is not editable metadata.
            if (Key == TEXT("ModName")) Descriptor.FriendlyName = Text;
            else if (Key == TEXT("ModDescription")) Descriptor.Description = Text;
            else if (Key == TEXT("ModVersion"))
            {
                Descriptor.VersionName = Text;
                Descriptor.Version = FCString::Atoi(*Text);
                Descriptor.AdditionalFieldsToWrite.Add(TEXT("SemVersion"), MakeShared<FJsonValueString>(Text));
            }
            else SetExtra(Key, Key);
        }
        Descriptor.AdditionalFieldsToWrite.Remove(TEXT("ModDependencies"));
        Descriptor.AdditionalFieldsToRemove.Add(TEXT("ModDependencies"));
        if (!Plugin.UpdateDescriptor(Descriptor, Reason)) { Error = Reason.ToString(); return false; }
        return true;
    }

    void Report(const FString& Error)
    {
        UE_LOG(LogCrimeFactory, Error, TEXT("Metadata synchronization: %s"), *Error);
        if (FSlateApplication::IsInitialized() && !FApp::IsUnattended())
        {
            FNotificationInfo Info(FText::FromString(TEXT("CrimeFactory could not synchronize mod metadata. ") + Error));
            Info.ExpireDuration = 12;
            FSlateNotificationManager::Get().AddNotification(Info);
        }
    }

    TSharedPtr<IPlugin> PluginForAsset(UObject* Asset)
    {
        if (!IsValid(Asset) || Asset->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !Asset->IsAsset()) return nullptr;
        const FString Name = Asset->GetOutermost()->GetName();
        TArray<FString> Parts;
        Name.ParseIntoArray(Parts, TEXT("/"), true);
        if (Parts.Num() != 2 || Parts[1] != TEXT("DA_") + Parts[0]) return nullptr;
        auto Plugin = IPluginManager::Get().FindPlugin(Parts[0]);
        if (!Plugin || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()),
            FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Mods")))) return nullptr;
        if (!FindField(*Asset, TEXT("ModID")) || !FindField(*Asset, TEXT("ModName"))) return nullptr;
        return Plugin;
    }

    void OnLoaded(UObject* Asset)
    {
        if (!IsInGameThread() || bSynchronizing) return;
        if (const auto Plugin = PluginForAsset(Asset))
        {
            auto& State = States.FindOrAdd(Plugin->GetName());
            State.Asset = Asset;
            State.AssetSnapshot = ReadAsset(*Asset);
            Pending.Add(Plugin->GetName()); // Never modify or save a package in its load callback.
        }
    }

    void OnChanged(UObject* Asset, FPropertyChangedEvent& Event)
    {
        if (!IsInGameThread() || bSynchronizing || (Event.ChangeType & EPropertyChangeType::Interactive)) return;
        if (const auto Plugin = PluginForAsset(Asset))
        {
            auto& State = States.FindOrAdd(Plugin->GetName());
            if (!State.AssetSnapshot || State.Asset != Asset)
            {
                // Newly created DAs need not have emitted OnAssetLoaded. Honor the field
                // the user just edited instead of resetting it on first reconciliation.
                State.Asset = Asset;
                State.AssetSnapshot = ReadAsset(*Asset);
                const auto Defaults = ReadDescriptor(*Plugin, *State.AssetSnapshot);
                FProperty* Changed = Event.MemberProperty ? Event.MemberProperty : Event.Property;
                for (const auto& Pair : Defaults->Values)
                    if (!Changed || FindField(*Asset, Pair.Key) == Changed) State.AssetSnapshot->SetField(Pair.Key, Pair.Value);
                if (Changed == FindField(*Asset, TEXT("Dependencies"))) State.AssetSnapshot->Values.Remove(TEXT("Dependencies"));
            }
            FString Error;
            if (!CrimeFactoryMetadataSync::SynchronizeAsset(*Plugin, *Asset, false, Error)) Report(Error);
        }
    }

    void OnTransacted(UObject* Asset, const FTransactionObjectEvent& Event)
    {
        if (!IsInGameThread() || bSynchronizing || Event.GetEventType() != ETransactionObjectEventType::UndoRedo) return;
        if (const auto Plugin = PluginForAsset(Asset)) Pending.Add(Plugin->GetName());
    }
}

bool CrimeFactoryMetadataSync::SynchronizeAsset(IPlugin& Plugin, UObject& Asset, bool bSaveAsset, FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();
    if (bSynchronizing) return true;
    TGuardValue<bool> Guard(bSynchronizing, true);
    if (!ReloadDescriptor(Plugin, OutError)) return false;
    if (Asset.GetOutermost()->HasAnyPackageFlags(PKG_Cooked | PKG_FilterEditorOnly))
    { OutError = TEXT("The mod metadata is cooked/read-only: ") + Asset.GetPathName(); return false; }
    auto& State = States.FindOrAdd(Plugin.GetName());
    auto Current = ReadAsset(Asset);
    if (!Current->HasField(TEXT("ModID")) || !Current->HasField(TEXT("ModName")))
    { OutError = TEXT("Expected PDA_ModMeta fields are missing from ") + Asset.GetPathName(); return false; }
    TSet<FString> Edited;
    if (State.Asset == &Asset && State.AssetSnapshot)
        for (const auto& Field : Current->Values)
            if (!Equal(Field.Value, State.AssetSnapshot->Values.FindRef(Field.Key))) Edited.Add(Field.Key);
    if (!Edited.IsEmpty() && !WriteDescriptor(Plugin, *Current, Edited, OutError)) return false;
    const auto Desired = ReadDescriptor(Plugin, *Current);
    // Resolve and type-check the complete dependency list before changing any DA fields.
    TArray<TStrongObjectPtr<UObject>> DependencyObjects;
    if (!ResolveDependencies(Plugin, Asset, DependencyObjects, OutError)) return false;
    if (bSaveAsset)
        for (const auto& Object : DependencyObjects)
            if (!FPackageName::DoesPackageExist(Object->GetPackage()->GetName()))
            {
                OutError = Plugin.GetName() + TEXT(": save dependency metadata ") + Object->GetPathName() + TEXT(" before saving/forging this mod.");
                return false;
            }
    TArray<TSharedPtr<FJsonValue>> DependencyPaths;
    for (const auto& Object : DependencyObjects) DependencyPaths.Add(MakeShared<FJsonValueString>(Object->GetPathName()));
    Desired->SetArrayField(TEXT("Dependencies"), DependencyPaths);
    bool bModified = false;
    for (const auto& Field : Desired->Values)
    {
        if (Equal(Field.Value, Current->Values.FindRef(Field.Key))) continue;
        FProperty* Property = FindField(Asset, Field.Key);
        if (!Property) continue;
        if (!bModified) { Asset.Modify(); bModified = true; }
        void* Ptr = Property->ContainerPtrToValuePtr<void>(&Asset);
        bool bWritten = false;
        if (auto* Array = CastField<FArrayProperty>(Property); Array && Field.Value->Type == EJson::Array)
        {
            if (Field.Key == TEXT("Dependencies"))
            {
                auto* Inner = CastFieldChecked<FObjectPropertyBase>(Array->Inner);
                FScriptArrayHelper Helper(Array, Ptr);
                Helper.EmptyAndAddValues(DependencyObjects.Num());
                for (int32 Index = 0; Index < Helper.Num(); ++Index)
                    Inner->SetObjectPropertyValue(Helper.GetRawPtr(Index), DependencyObjects[Index].Get());
                bWritten = true;
            }
            else if (CastField<FStrProperty>(Array->Inner) || CastField<FTextProperty>(Array->Inner) || CastField<FNameProperty>(Array->Inner))
            {
                FScriptArrayHelper Helper(Array, Ptr);
                Helper.EmptyAndAddValues(Field.Value->AsArray().Num());
                bWritten = true;
                for (int32 Index = 0; Index < Helper.Num(); ++Index)
                    bWritten &= WriteText(Array->Inner, Helper.GetRawPtr(Index), Field.Value->AsArray()[Index]->AsString());
            }
        }
        else if (Field.Value->Type == EJson::String) bWritten = WriteText(Property, Ptr, Field.Value->AsString());
        if (!bWritten)
        {
            OutError = FString::Printf(TEXT("%s: '%s' has an unsupported type or enum value. Repair the mod properties before forging."), *Asset.GetPathName(), *Field.Key);
            return false;
        }
        FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
        Asset.PostEditChangeProperty(Event);
    }
    State.Asset = &Asset;
    State.AssetSnapshot = ReadAsset(Asset);
    FFileHelper::LoadFileToString(State.DiskText, *Plugin.GetDescriptorFileName());
    Pending.Remove(Plugin.GetName());
    if (bModified) Asset.MarkPackageDirty();
    if (bSaveAsset && Asset.GetOutermost()->IsDirty())
    {
        auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
        if (!Subsystem || !Subsystem->SaveLoadedAsset(&Asset))
        { OutError = TEXT("Could not save synchronized metadata: ") + Asset.GetPathName() + TEXT(". Check file permissions/source control before forging."); return false; }
    }
    return true;
}

bool CrimeFactoryMetadataSync::Synchronize(IPlugin& Plugin, bool bSaveAsset, FString& OutError)
{
    OutError.Reset();
    if (const auto* State = States.Find(Plugin.GetName()); State && State->Asset.IsValid())
        return SynchronizeAsset(Plugin, *State->Asset, bSaveAsset, OutError);
    const FString Package = Plugin.GetMountedAssetPath() / (TEXT("DA_") + Plugin.GetName());
    if (!FPackageName::DoesPackageExist(Package)) return true; // Content-only mods need not have PDML metadata.
    UObject* Asset;
    {
        TGuardValue<bool> Guard(bSynchronizing, true);
        Asset = LoadObject<UObject>(nullptr, *(Package + TEXT(".") + TEXT("DA_") + Plugin.GetName()));
    }
    if (!Asset) { OutError = TEXT("Cannot load mod metadata: ") + Package; return false; }
    return SynchronizeAsset(Plugin, *Asset, bSaveAsset, OutError);
}

bool CrimeFactoryMetadataSync::ReloadDescriptor(IPlugin& Plugin, FString& OutError)
{
    OutError.Reset();
    FPluginDescriptor Descriptor;
    FText Reason;
    if (!Descriptor.Load(Plugin.GetDescriptorFileName(), Reason)) { OutError = Reason.ToString(); return false; }
    const auto Before = DescriptorJson(Plugin.GetDescriptor());
    const auto After = DescriptorJson(Descriptor);
    if (FJsonValue::CompareEqual(FJsonValueObject(Before), FJsonValueObject(After))) return true;
    if (!Plugin.UpdateDescriptor(Descriptor, Reason)) { OutError = Reason.ToString(); return false; }
    return true;
}

void CrimeFactoryMetadataSync::PopulateProperties(IPlugin& Plugin, UModMetadataObject& Properties)
{
    FString Error;
    if (!Synchronize(Plugin, false, Error)) { Report(Error); return; }
    const auto* State = States.Find(Plugin.GetName());
    const auto Values = ReadDescriptor(Plugin, State && State->AssetSnapshot ? *State->AssetSnapshot : FJsonObject());
    Properties.ModAuthors.Reset();
    Properties.ModDependencies.Reset();
    Values->TryGetStringArrayField(TEXT("ModAuthors"), Properties.ModAuthors);
    for (const auto& Reference : Plugin.GetDescriptor().Plugins)
        if (Reference.bEnabled && !IsBaseDependency(Reference))
            if (const auto Dependency = IPluginManager::Get().FindPlugin(Reference.Name); Dependency && IsProjectMod(*Dependency))
                Properties.ModDependencies.AddUnique(Reference.Name);
    Values->TryGetStringField(TEXT("ModPageURL"), Properties.ModPageURL);
    Values->TryGetStringField(TEXT("ModEnvironment"), Properties.ModEnvironment);
    Values->TryGetStringField(TEXT("ModLoaderVersion"), Properties.ModLoaderVersion);
}

void CrimeFactoryMetadataSync::FlushPending()
{
    if (!GEditor || IsGarbageCollecting() || bSynchronizing) return;
    const auto Work = Pending.Array();
    for (const auto& Name : Work)
    {
        Pending.Remove(Name);
        if (const auto Plugin = IPluginManager::Get().FindPlugin(Name))
        {
            FString Error;
            if (const auto* State = States.Find(Name); State && State->Asset.IsValid())
                if (!SynchronizeAsset(*Plugin, *State->Asset, false, Error)) Report(Error);
        }
    }
}

void CrimeFactoryMetadataSync::Initialize()
{
    if (IsRunningCommandlet() || TickHandle.IsValid()) return;
    MetadataPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddStatic(&OnChanged);
    TransactionHandle = FCoreUObjectDelegates::OnObjectTransacted.AddStatic(&OnTransacted);
    LoadHandle = FCoreUObjectDelegates::OnAssetLoaded.AddStatic(&OnLoaded);
    SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([](UObject* Object, FObjectPreSaveContext Context)
    {
        if (!IsInGameThread() || Context.IsCooking() || bSynchronizing) return;
        if (const auto Plugin = PluginForAsset(Object))
        {
            FString Error;
            if (!SynchronizeAsset(*Plugin, *Object, false, Error)) Report(Error);
        }
    });
    PluginHandle = IPluginManager::Get().OnPluginEdited().AddLambda([](IPlugin& Plugin)
    {
        if (bSynchronizing) return;
        FString Error;
        if (!Synchronize(Plugin, true, Error)) Report(Error);
    });
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
    {
        if (!GEditor || IsGarbageCollecting() || bSynchronizing) return true;
        FlushPending();
        // Check only tracked descriptor files, not the asset tree. This also catches external config edits.
        for (auto& Pair : States)
        {
            if (const auto Plugin = IPluginManager::Get().FindPlugin(Pair.Key))
            {
                FString Text;
                if (FFileHelper::LoadFileToString(Text, *Plugin->GetDescriptorFileName()) && Text != Pair.Value.DiskText)
                {
                    Pair.Value.DiskText = Text;
                    FPluginDescriptor Descriptor;
                    FText Reason;
                    TGuardValue<bool> Guard(bSynchronizing, true);
                    if (!Descriptor.Read(Text, Reason) || !Plugin->UpdateDescriptor(Descriptor, Reason)) { Report(Reason.ToString()); continue; }
                    Pending.Add(Pair.Key);
                }
            }
        }
        return true;
    }), 0.5f);
}

void CrimeFactoryMetadataSync::Shutdown()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    TickHandle.Reset();
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(MetadataPropertyChangedHandle);
    FCoreUObjectDelegates::OnObjectTransacted.Remove(TransactionHandle);
    FCoreUObjectDelegates::OnAssetLoaded.Remove(LoadHandle);
    FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle);
    IPluginManager::Get().OnPluginEdited().Remove(PluginHandle);
    Pending.Reset();
    States.Reset();
}
