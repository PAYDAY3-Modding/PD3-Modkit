/**
 * @file SBZUITextBlock.cpp
 * @brief Applies SBZUITextBlock styles through Suzie native bodies and editor callbacks.
 *
 * The reflected class still comes from the JMAP. These bodies provide its editor behaviour
 * without declaring a separate native Starbreeze class.
 */

#include "Components/TextBlock.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateColor.h"
#include "Layout/Margin.h"

namespace SuzieUITextBlock
{
    /** Get the widget's font and layout style through reflection. */
    static UObject* GetStyle(FSuzieFunctionContext& Context) { return Context.GetProperty<UObject*>("Style"); }

    /** Get the asset that supplies the widget's text colour. */
    static UObject* GetColorStyle(FSuzieFunctionContext& Context) { return Context.GetProperty<UObject*>("ColorStyle"); }

    /**
     * Check that a style asset is ready before reading its properties.
     *
     * @param Object Style or colour asset to check. May be null.
     * @return True when the asset is valid and initialization and loading have finished.
     */
    static bool CanReadStyle(UObject* Object)
    {
        return IsValid(Object) && !Object->HasAnyFlags(RF_NeedInitialization | RF_NeedLoad |
            RF_NeedPostLoad | RF_NeedPostLoadSubobjects) &&
            !Object->HasAnyInternalFlags(EInternalObjectFlags_AsyncLoading | EInternalObjectFlags::Async);
    }

    /**
     * Apply the assigned styles using the normal UTextBlock setters.
     *
     * Rebuild asset subscriptions so edits to the current styles refresh this widget.
     * Unassigned styles leave the widget's current values in place.
     *
     * @param Context Access to the widget and its reflected style references.
     */
    static void UpdateTextProperties(FSuzieFunctionContext& Context)
    {
        // Styling is for editor previews. Leave class defaults and cook output alone.
        if (Context.IsClassDefaultObject() || IsRunningCommandlet()) { return; }
        UTextBlock* TextBlock = Cast<UTextBlock>(Context.GetSelf());
        if (!TextBlock)
        {
            Context.Fail(TEXT("SBZUITextBlock requires a UTextBlock native parent"));
            return;
        }

        // Drop subscriptions to old assets before watching the current style references.
        Context.ClearDependencies();
        Context.ObserveWidget();
        UObject* Style = GetStyle(Context);
        if (IsValid(Style))
        {
            for (FName Property : { FName(TEXT("Font")), FName(TEXT("Margin")), FName(TEXT("bEnableShadows")),
                FName(TEXT("ShadowColor")), FName(TEXT("ShadowOffset")), FName(TEXT("LineHeightPercentage")) })
            { Context.WatchDependency(Style, Property); }
        }
        // Subscribe before checking readiness so an asset that is still loading can refresh the widget once it is ready.
        if (CanReadStyle(Style))
        {
            const FSlateFontInfo Font = Context.Property("Font", Style).Get<FSlateFontInfo>();
            if (!Context.IsValid()) { return; }
            if (!(TextBlock->GetFont() == Font)) { TextBlock->SetFont(Font); }
            const FMargin Margin = Context.Property("Margin", Style).Get<FMargin>();
            const float LineHeight = Context.Property("LineHeightPercentage", Style).Get<float>();
            const bool bShadows = Context.Property("bEnableShadows", Style).Get<bool>();
            const FVector2D Offset = bShadows ? Context.Property("ShadowOffset", Style).Get<FVector2D>() : FVector2D::ZeroVector;
            if (!Context.IsValid()) { return; }
            if (!(Context.GetProperty<FMargin>("Margin") == Margin)) { TextBlock->SetMargin(Margin); }
            if (Context.GetProperty<float>("LineHeightPercentage") != LineHeight) { TextBlock->SetLineHeightPercentage(LineHeight); }
            if (TextBlock->GetShadowOffset() != Offset) { TextBlock->SetShadowOffset(Offset); }
            if (bShadows)
            {
                if (UObject* Shadow = Context.Property("ShadowColor", Style).Get<UObject*>(); IsValid(Shadow))
                {
                    // The shadow colour is a separate asset and needs its own subscription.
                    Context.WatchDependency(Shadow, TEXT("Value"));
                    if (CanReadStyle(Shadow))
                    {
                        const FLinearColor Tint = Context.Property("Value", Shadow).Get<FLinearColor>();
                        if (Context.IsValid() && TextBlock->GetShadowColorAndOpacity() != Tint) { TextBlock->SetShadowColorAndOpacity(Tint); }
                    }
                }
            }
        }
        UObject* ColorStyle = GetColorStyle(Context);
        if (IsValid(ColorStyle)) { Context.WatchDependency(ColorStyle, TEXT("Value")); }
        if (CanReadStyle(ColorStyle))
        {
            const FLinearColor Value = Context.Property("Value", ColorStyle).Get<FLinearColor>();
            if (!Context.IsValid()) { return; }
            if (TextBlock->GetColorAndOpacity() != FSlateColor(Value)) { TextBlock->SetColorAndOpacity(FSlateColor(Value)); }
        }
    }

}

/**
 * Set starting values before Suzie restores the object's defaults.
 * JMAP, archetype and saved values take precedence over this early setup.
 */
SUZIE_CONSTRUCT(SBZUITextBlock)
{
    Context.SetProperty<UObject*>("ColorStyle", nullptr);
    Context.SetProperty<UObject*>("Style", nullptr);
    Context.SetProperty<UClass*>("ScrollStyle", nullptr);
    Context.SetProperty<bool>("bIsScrollingEnabled", false);
    Context.SetProperty<bool>("bEnableHideTextOnEmpty", false);
    Context.SetProperty<ESlateVisibility>("OnEmptyVisibility", ESlateVisibility::Collapsed);
    Context.SetProperty<ESlateVisibility>("OnNotEmptyVisbility", ESlateVisibility::SelfHitTestInvisible);
}

/** Apply the styles after the widget's defaults and required loading are ready. */
SUZIE_POST_INITIALIZE(SBZUITextBlock)
{
    SuzieUITextBlock::UpdateTextProperties(Context);
}

/** Refresh font, layout and shadow settings when Style changes in the editor. */
SUZIE_PROPERTY_CHANGED(SBZUITextBlock, Style)
{
    SuzieUITextBlock::UpdateTextProperties(Context);
}

/** Refresh the text colour when the ColorStyle reference changes in the editor. */
SUZIE_PROPERTY_CHANGED(SBZUITextBlock, ColorStyle)
{
    SuzieUITextBlock::UpdateTextProperties(Context);
}

/**
 * Reapply styles after a watched asset changes or the widget's Slate representation is replaced.
 * Suzie queues this callback; it does not override RebuildWidget or SynchronizeProperties.
 */
SUZIE_REFRESH(SBZUITextBlock)
{
    SuzieUITextBlock::UpdateTextProperties(Context);
}

/** Assign the text style and apply it to the widget. */
SUZIE_FUNCTION(SBZUITextBlock, SetStyle)
{
    UObject* InStyle = Context.GetParam<UObject*>("InStyle");
    if (Context.IsValid() && Context.SetProperty<UObject*>("Style", InStyle))
    {
        SuzieUITextBlock::UpdateTextProperties(Context);
    }
}

/** Store the requested colour style and update the text colour. */
SUZIE_FUNCTION(SBZUITextBlock, SetColorStyle)
{
    UObject* InStyle = Context.GetParam<UObject*>("InStyle");
    if (Context.IsValid() && Context.SetProperty<UObject*>("ColorStyle", InStyle))
    {
        SuzieUITextBlock::UpdateTextProperties(Context);
    }
}

/**
 * Store the requested scrolling flag.
 * @note This only updates the flag. Scrolling behaviour is not implemented here.
 */
SUZIE_FUNCTION(SBZUITextBlock, SetScrollingEnabled)
{
    const bool bEnabled = Context.GetParam<bool>("bInIsScrollingEnabled");
    if (Context.IsValid()) { Context.SetProperty<bool>("bIsScrollingEnabled", bEnabled); }
}
