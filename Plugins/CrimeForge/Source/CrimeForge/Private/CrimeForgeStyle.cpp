#include "CrimeForgeStyle.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleRegistry.h"

static FName StyleSetName(TEXT("CrimeForgeStyle"));
TSharedPtr< FSlateStyleSet > FCrimeForgeStyle::StyleInstance = NULL;

void FCrimeForgeStyle::Initialize() {
    if (!StyleInstance.IsValid()) {
        StyleInstance = Create();
        FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
    }
}

void FCrimeForgeStyle::Shutdown() {
    FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
    ensure(StyleInstance.IsUnique());
    StyleInstance.Reset();
}

FName FCrimeForgeStyle::GetStyleSetName() {
    return StyleSetName;
}

#define IMAGE_BRUSH(RelativePath, ...) FSlateImageBrush( Style->RootToContentDir( RelativePath, TEXT(".png") ), __VA_ARGS__)
#define BOX_BRUSH(RelativePath, ...) FSlateBoxBrush( Style->RootToContentDir( RelativePath, TEXT(".png") ), __VA_ARGS__)
#define BORDER_BRUSH(RelativePath, ...) FSlateBorderBrush( Style->RootToContentDir( RelativePath, TEXT(".png") ), __VA_ARGS__)
#define TTF_FONT(RelativePath, ...) FSlateFontInfo( Style->RootToContentDir( RelativePath, TEXT(".ttf") ), __VA_ARGS__)
#define OTF_FONT(RelativePath, ...) FSlateFontInfo( Style->RootToContentDir( RelativePath, TEXT(".otf") ), __VA_ARGS__)

const FVector2D Icon16x16(16.0f, 16.0f);
const FVector2D Icon20x20(20.0f, 20.0f);
const FVector2D Icon40x40(40.0f, 40.0f);
const FVector2D Icon128x128(40.0f, 40.0f);

TSharedRef<FSlateStyleSet> FCrimeForgeStyle::Create() {
    TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet(StyleSetName));
    Style->SetContentRoot(IPluginManager::Get().FindPlugin(TEXT("CrimeForge"))->GetBaseDir() / TEXT("Resources"));
    Style->Set(TEXT("CrimeForge.OpenPluginWindow"), new IMAGE_BRUSH(TEXT("CrimeForge40"), Icon40x40));
    return Style;
}

void FCrimeForgeStyle::ReloadTextures() {
    if (FSlateApplication::IsInitialized()) {
        FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
    }
}

const ISlateStyle& FCrimeForgeStyle::Get() {
    return *StyleInstance;
}
