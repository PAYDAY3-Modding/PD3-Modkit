#include "AssetPalette/SCrimeFactoryPaletteTile.h"

#include "AssetThumbnail.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SCrimeFactoryPaletteTile::Construct(const FArguments& Args, const TSharedRef<STableViewBase>& Owner)
{
    const auto Item = Args._Item;
    const FSlateFontInfo NameFont = FAppStyle::GetFontStyle("NormalFont");
    const float NameHeight = 3.0f * FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->GetMaxCharacterHeight(NameFont);
    TSharedRef<SWidget> Image = SNew(SImage).Image(FAppStyle::GetBrush(
        !Item->Problem.IsEmpty() ? "Icons.WarningWithColor" : "ClassIcon.Actor"));
    const bool bActorPreview = CrimeFactoryActorThumbnails::ShouldUsePreview(Item->AssetData, Item->bClassEntry);
    if ((Item->AssetData.IsValid() || bActorPreview) && Item->Problem.IsEmpty())
    {
        // Only virtualized, visible rows own thumbnail requests. The pool is bounded.
        FAssetData ThumbnailData = Item->AssetData;
        if (bActorPreview)
        {
            ActorThumbnail.Reset(NewObject<UCrimeFactoryActorThumbnail>(GetTransientPackage(), NAME_None, RF_Transient));
            ActorThumbnail->SourcePath = Item->Path;
            ThumbnailData = FAssetData(ActorThumbnail.Get());
        }
        Thumbnail = MakeShared<FAssetThumbnail>(ThumbnailData, 96, 96, Args._ThumbnailPool);
        FAssetThumbnailConfig Config;
        Config.bAllowRealTimeOnHovered = false;
        Config.bAllowHintText = false;
        Config.ThumbnailLabel = EThumbnailLabel::NoLabel;
        if (bActorPreview) Config.ClassThumbnailBrushOverride = "ClassThumbnail.Blueprint";
        Image = Thumbnail->MakeThumbnailWidget(Config);
    }
    STableRow::Construct(STableRow::FArguments()
        .Padding(6)
        .OnDragDetected(Args._OnAssetDragDetected)
        .ToolTipText(Item->GetTooltip()), Owner);
    SetContent(SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
        [ SNew(SBox).WidthOverride(96).HeightOverride(96)[Image] ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 0)
        [ SNew(SBox).MaxDesiredHeight(NameHeight).Clipping(EWidgetClipping::ClipToBounds)
            [ SNew(STextBlock).Text(Item->Label).Font(NameFont).Justification(ETextJustify::Center)
                .AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
                .OverflowPolicy(ETextOverflowPolicy::Ellipsis) ] ]
        + SVerticalBox::Slot().AutoHeight()
        [ SNew(STextBlock).Text(Item->Problem)
            .Visibility(Item->Problem.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
            .Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
            .Justification(ETextJustify::Center).OverflowPolicy(ETextOverflowPolicy::Ellipsis) ]);
}

FReply SCrimeFactoryPaletteTile::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
    FReply Reply = STableRow::OnMouseButtonDown(Geometry, Event);
    if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
        return Reply.DetectDrag(SharedThis(this), EKeys::LeftMouseButton);
    return Reply;
}
