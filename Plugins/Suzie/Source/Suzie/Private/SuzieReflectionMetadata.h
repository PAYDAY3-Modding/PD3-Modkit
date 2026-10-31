#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FProperty;
class UEnum;
class UClass;
class UFunction;

namespace SuzieReflectionMetadata
{
    // Plugin Config/SuzieMetadata.json supplies bundled defaults; dumped metadata
    // takes precedence. Optional project Config/SuzieMetadata.json wins over both.
    // properties[path] is a
    // string metadata map; classes[path] and functions[path] use the same form.
    // enums[path] has metadata and entry_metadata[name] maps.
    TSharedPtr<FJsonObject> LoadOverrides();
    void ApplyClass(UClass* Class, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides);
    void ApplyFunction(UFunction* Function, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides);
    void ApplyProperty(FProperty* Property, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides);
    void ApplyEnum(UEnum* Enum, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides);
}
