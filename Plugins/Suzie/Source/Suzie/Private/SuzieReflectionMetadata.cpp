#include "SuzieReflectionMetadata.h"
#include "SuziePlugin.h"
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

    TSharedPtr<FJsonObject> LoadOverrides()
    {
        const FString Path = FPaths::ProjectConfigDir() / TEXT("SuzieMetadata.json");
        if (!FPaths::FileExists(Path)) return nullptr;
        FString Json;
        TSharedPtr<FJsonObject> Result;
        if (!FFileHelper::LoadFileToString(Json, *Path)
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result) || !Result.IsValid())
        {
            UE_LOG(LogSuzie, Error, TEXT("Cannot read metadata overrides: %s"), *Path);
            return nullptr;
        }
        return Result;
    }

    void ApplyProperty(FProperty* Property, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides)
    {
        const auto Set = [Property](const TCHAR* Key, const TCHAR* Value) { Property->SetMetaData(Key, Value); };
        ApplyMap(ObjectField(Definition, TEXT("metadata")), Set);
        ApplyMap(ObjectField(ObjectField(Overrides, TEXT("properties")), Property->GetPathName()), Set);
    }

    void ApplyEnum(UEnum* Enum, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides)
    {
        const auto Apply = [Enum](const TSharedPtr<FJsonObject>& Source)
        {
            ApplyMap(ObjectField(Source, TEXT("metadata")),
                [Enum](const TCHAR* Key, const TCHAR* Value) { Enum->SetMetaData(Key, Value); });
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
                ApplyMap(Entry.Value->AsObject(), [Enum, Index](const TCHAR* Key, const TCHAR* Value)
                    { Enum->SetMetaData(Key, Value, Index); });
            }
        };
        Apply(Definition);
        Apply(ObjectField(ObjectField(Overrides, TEXT("enums")), Enum->GetPathName()));
    }
}
