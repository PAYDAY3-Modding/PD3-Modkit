#include "CrimeFactoryActorVisualisationSettings.h"

#include "CrimeFactoryPawnPreview.h"
#include "UObject/ICookInfo.h"
#include "UObject/UnrealType.h"

TArray<FString> UCrimeFactoryActorVisualisationSettings::GetPawnRequestPropertyOptions() const
{
    TArray<FString> Options;
    FCookLoadScope EditorOnlyLoadScope(ECookLoadType::EditorOnly);
    for (const auto& Definition : PawnSpawnerDefinitions)
    {
        if (UClass* Class = Definition.ActorClass.LoadSynchronous())
        {
            for (TFieldIterator<FProperty> It(Class); It; ++It)
            {
                if (FCrimeFactoryPawnPreview::SupportsRequestProperty(*It)) { Options.AddUnique(It->GetName()); }
            }
        }
    }
    Options.Sort();
    return Options;
}
