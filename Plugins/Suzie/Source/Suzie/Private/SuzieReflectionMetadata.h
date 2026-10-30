#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FProperty;
class UEnum;

namespace SuzieReflectionMetadata
{
    // Optional project Config/SuzieMetadata.json: properties[path] is a string
    // metadata map; enums[path] has metadata and entry_metadata[name] maps.
    TSharedPtr<FJsonObject> LoadOverrides();
    void ApplyProperty(FProperty* Property, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides);
    void ApplyEnum(UEnum* Enum, const TSharedPtr<FJsonObject>& Definition,
        const TSharedPtr<FJsonObject>& Overrides);
}
