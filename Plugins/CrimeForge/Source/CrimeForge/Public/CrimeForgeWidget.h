#pragma once

#include "CrimeForgeModEntryList.h"
#include "Slate.h"

class SCrimeForgeWidget : public SCompoundWidget {
public:
    SLATE_BEGIN_ARGS(SCrimeForgeWidget) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    TSharedPtr<IDetailsView> DetailsView;
    TSharedPtr<SCrimeForgeModEntryList> ModList;
};
