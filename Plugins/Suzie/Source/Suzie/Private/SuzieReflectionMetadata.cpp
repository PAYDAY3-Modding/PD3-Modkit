#include "SuzieReflectionMetadata.h"
#include "SuziePlugin.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

namespace SuzieReflectionMetadata
{
    static TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const FString& Key)
    {
        const TSharedPtr<FJsonObject>* Result = nullptr;
        return Object.IsValid() && Object->TryGetObjectField(Key, Result) ? *Result : nullptr;
    }

    template<typename Setter>
    static void ApplyMap(const TSharedPtr<FJsonObject>& Map, Setter Set)
    {
        if (!Map.IsValid()) return;
        for (const auto& Pair : Map->Values)
        {
            if (Pair.Value->Type == EJson::String)
            {
                Set(*Pair.Key, *Pair.Value->AsString());
            }
            else
            {
                UE_LOG(LogSuzie, Warning, TEXT("Metadata '%s' must be a string (use an empty string for presence-only metadata)"), *Pair.Key);
            }
        }
    }

    static TSharedPtr<FJsonObject> LoadMetadataFile(const FString& Path)
    {
        if (!FPaths::FileExists(Path)) return nullptr;
        FString Json;
        TSharedPtr<FJsonObject> Result;
        if (!FFileHelper::LoadFileToString(Json, *Path)
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result) || !Result.IsValid())
        {
            UE_LOG(LogSuzie, Error, TEXT("Cannot read metadata overrides: %s"), *Path);
            return nullptr;
        }
        UE_LOG(LogSuzie, Display, TEXT("Loaded reflection metadata: %s"), *Path);
        return Result;
    }

    static void MergeMetadata(const TSharedPtr<FJsonObject>& Destination, const TSharedPtr<FJsonObject>& Source)
    {
        if (!Source.IsValid()) return;
        for (const auto& Pair : Source->Values)
        {
            const TSharedPtr<FJsonObject> Existing = ObjectField(Destination, Pair.Key);
            if (Existing.IsValid() && Pair.Value->Type == EJson::Object)
            {
                MergeMetadata(Existing, Pair.Value->AsObject());
            }
            else
            {
                Destination->SetField(Pair.Key, Pair.Value);
            }
        }
    }

    TSharedPtr<FJsonObject> LoadOverrides()
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        // Keep verified mappings with the plugin so a source ZIP or BuildPlugin
        // package is sufficient. Project overrides are optional and take precedence
        // per metadata key, rather than discarding the rest of a bundled mapping.
        if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Suzie")))
        {
            MergeMetadata(Result, LoadMetadataFile(Plugin->GetBaseDir() / TEXT("Config/SuzieMetadata.json")));
        }
        const TSharedPtr<FJsonObject> Project = LoadMetadataFile(FPaths::ProjectConfigDir() / TEXT("SuzieMetadata.json"));
        MergeMetadata(Result, Project);
        if (Project.IsValid()) Result->SetObjectField(TEXT("$project_overrides"), Project);
        return Result;
    }

    static void ApplyField(UField* Field, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides, const TCHAR* Section)
    {
        const auto Set = [Field](const TCHAR* Key, const TCHAR* Value) { Field->SetMetaData(Key, Value); };
        ApplyMap(ObjectField(ObjectField(Overrides, Section), Field->GetPathName()), Set);
        ApplyMap(ObjectField(Definition, TEXT("metadata")), Set);
        ApplyMap(ObjectField(ObjectField(ObjectField(Overrides, TEXT("$project_overrides")), Section), Field->GetPathName()), Set);
    }

    void ApplyClass(UClass* Class, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides)
    {
        ApplyField(Class, Definition, Overrides, TEXT("classes"));
    }

    void ApplyFunction(UFunction* Function, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides)
    {
        ApplyField(Function, Definition, Overrides, TEXT("functions"));
    }

    void ApplyProperty(FProperty* Property, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides)
    {
        const auto Set = [Property](const TCHAR* Key, const TCHAR* Value) { Property->SetMetaData(Key, Value); };
        ApplyMap(ObjectField(ObjectField(Overrides, TEXT("properties")), Property->GetPathName()), Set);
        ApplyMap(ObjectField(Definition, TEXT("metadata")), Set);
        ApplyMap(ObjectField(ObjectField(ObjectField(Overrides, TEXT("$project_overrides")), TEXT("properties")), Property->GetPathName()), Set);
        // UHT emits EditInline metadata for Instanced strong UObject references.
        // Shipping reflection retains the storage/ownership flags but strips this
        // metadata. PropertyNode uses it to select the stock inline class picker.
        // BuildProperty visits container elements and struct members too. Do not
        // turn weak/soft/class references or ordinary object references into owners.
        if (Property->IsA<FObjectProperty>() && !Property->IsA<FClassProperty>()
            && Property->HasAnyPropertyFlags(CPF_InstancedReference)
            && !Property->HasMetaData(TEXT("NoEditInline"))
            && !Property->HasMetaData(TEXT("EditInline")))
        {
            Property->SetMetaData(TEXT("EditInline"), TEXT("true"));
        }
    }

    void ApplyEnum(UEnum* Enum, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides)
    {
        const auto Apply = [Enum](const TSharedPtr<FJsonObject>& Source, bool bFromDump = false)
        {
            const TSharedPtr<FJsonObject> Metadata = ObjectField(Source, TEXT("metadata"));
            // Real native editor metadata outranks heuristically inferred settings.
            const bool bKeepMode = bFromDump && Metadata.IsValid()
                && Metadata->HasField(TEXT("JmapInferredMaskMode"))
                && Enum->HasMetaData(TEXT("UseEnumValuesAsMaskValuesInEditor"))
                && !Enum->HasMetaData(TEXT("JmapInferredMaskMode"));
            const bool bKeepFlags = bFromDump && Metadata.IsValid()
                && Metadata->HasField(TEXT("JmapInferredBitflags"))
                && Enum->HasMetaData(TEXT("Bitflags")) && !Enum->HasMetaData(TEXT("JmapInferredBitflags"));
            ApplyMap(Metadata, [Enum, bKeepMode, bKeepFlags](const TCHAR* Key, const TCHAR* Value)
            {
                if (bKeepMode && (FCString::Stricmp(Key, TEXT("UseEnumValuesAsMaskValuesInEditor")) == 0
                    || FCString::Stricmp(Key, TEXT("JmapInferredMaskMode")) == 0)) return;
                if (bKeepFlags && (FCString::Stricmp(Key, TEXT("Bitflags")) == 0
                    || FCString::Stricmp(Key, TEXT("JmapInferredBitflags")) == 0)) return;
                Enum->SetMetaData(Key, Value);
            });
            const TSharedPtr<FJsonObject> Entries = ObjectField(Source, TEXT("entry_metadata"));
            if (!Entries.IsValid()) return;
            for (const auto& Entry : Entries->Values)
            {
                // Match names, never ordinal positions or numeric values (aliases are legal).
                int32 Index = INDEX_NONE;
                for (int32 I = 0; I < Enum->NumEnums(); ++I)
                {
                    if (Enum->GetNameByIndex(I).ToString() == Entry.Key) { Index = I; break; }
                }
                if (Index == INDEX_NONE)
                {
                    UE_LOG(LogSuzie, Warning, TEXT("Unknown metadata enum entry %s in %s"), *Entry.Key, *Enum->GetPathName());
                    continue;
                }
                const TSharedPtr<FJsonObject> Values = Entry.Value->AsObject();
                const bool bKeepHidden = bFromDump && Values.IsValid() && Values->HasField(TEXT("JmapInferredHidden"))
                    && Enum->HasMetaData(TEXT("Hidden"), Index) && !Enum->HasMetaData(TEXT("JmapInferredHidden"), Index);
                ApplyMap(Values, [Enum, Index, bKeepHidden](const TCHAR* Key, const TCHAR* Value)
                {
                    if (bKeepHidden && (FCString::Stricmp(Key, TEXT("Hidden")) == 0
                        || FCString::Stricmp(Key, TEXT("JmapInferredHidden")) == 0)) return;
                    Enum->SetMetaData(Key, Value, Index);
                });
            }
        };
        Apply(ObjectField(ObjectField(Overrides, TEXT("enums")), Enum->GetPathName()));
        Apply(Definition, true);
        Apply(ObjectField(ObjectField(ObjectField(Overrides, TEXT("$project_overrides")), TEXT("enums")), Enum->GetPathName()));
    }
}
