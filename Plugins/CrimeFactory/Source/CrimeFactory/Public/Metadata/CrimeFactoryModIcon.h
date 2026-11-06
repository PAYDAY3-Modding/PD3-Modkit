#pragma once

#include "CoreMinimal.h"

struct FSlateDynamicImageBrush;

namespace CrimeFactoryModIcon
{
    /** Validate PNG bytes independently of Slate or the rendering backend. */
    bool IsValidPNG(const FString& Filename);

    /** Validate and copy a chosen PNG into a mod's Resources/Icon128.png. */
    bool CopyToResources(const FString& Filename, const FString& PluginDirectory, FText& Error);

    /** Decode current bytes into a uniquely named Slate resource, avoiding filename texture-cache staleness. */
    TSharedPtr<FSlateDynamicImageBrush> Load(const FString& Filename);
}
