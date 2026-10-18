#include "Packaging/CrimeFactoryUpdaterUI.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/App.h"
#include "HAL/PlatformProcess.h"
#include "Widgets/Input/SHyperlink.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

FString FCrimeFactoryUpdatePresentation::BuildReport() const
{
    return FString::Printf(TEXT("PD3 Modkit Updater\n\n%s\n\n%s\n\nBranch: %s\nRepository: %s\nCurrent: %s\nLatest: %s\nLatest commit: %s\nUpdates: %d\nLocal commits: %d\nLocal changes: %d"),
        *Status, *Detail, *Branch, *Repository, *CurrentCommit, *LatestCommit, *LatestSubject, CommitsBehind, CommitsAhead, LocalChanges)
        + FString::Printf(TEXT("\n\nUE5-PD3 Engine\n%s\nInstalled: %s\nLatest compatible: %s\nPath: %s\n%s\n%s"),
            *Engine.Status, *Engine.InstalledVersion, *Engine.LatestVersion, *Engine.EngineRoot, *Engine.Detail, *Engine.ReleaseURL);
}

TSharedRef<SWidget> CrimeFactoryUpdaterUI::MakeContent(const FCrimeFactoryUpdatePresentation& Data,
    const FSimpleDelegate& OnUpdate, const FSimpleDelegate& OnClose)
{
    const TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
    Body->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
        [SNew(STextBlock).Text(NSLOCTEXT("CrimeFactory", "ModkitComponent", "PAYDAY 3 Modkit")).Font(FAppStyle::GetFontStyle("BoldFont"))];
    Body->AddSlot().AutoHeight().Padding(0, 0, 0, 14)
        [CrimeFactoryPackagingUI::MakeMessagePanel(Data.Detail, Data.Severity)];
    for (const auto& Field : TArray<TPair<FString, FString>>{{TEXT("Branch"), Data.Branch}, {TEXT("Current commit"), Data.CurrentCommit},
        {TEXT("Latest commit"), Data.LatestCommit}, {TEXT("Latest change"), Data.LatestSubject}, {TEXT("Repository"), Data.Repository}})
    {
        if (Field.Value.IsEmpty()) continue;
        Body->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Field.Key)).Font(FAppStyle::GetFontStyle("BoldFont"))]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 3)[SNew(STextBlock).Text(FText::FromString(Field.Value)).AutoWrapText(true)]
        ];
    }
    if (!Data.Engine.Status.IsEmpty())
    {
        Body->AddSlot().AutoHeight().Padding(0, 18, 0, 10)
            [SNew(STextBlock).Text(NSLOCTEXT("CrimeFactory", "EngineComponent", "UE5_PD3 Engine")).Font(FAppStyle::GetFontStyle("BoldFont"))];
        Body->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
            [CrimeFactoryPackagingUI::MakeMessagePanel(Data.Engine.Status + TEXT("\n\n") + Data.Engine.Detail, Data.Engine.Severity)];
        Body->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
            [SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("Installed: %s    |    Latest compatible: %s\n%s"),
                Data.Engine.InstalledVersion.IsEmpty() ? TEXT("Unknown") : *Data.Engine.InstalledVersion,
                Data.Engine.LatestVersion.IsEmpty() ? TEXT("Unknown") : *Data.Engine.LatestVersion, *Data.Engine.EngineRoot))).AutoWrapText(true)];
        if (!Data.Engine.ReleaseURL.IsEmpty())
            Body->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
                [SNew(SHyperlink).Text(NSLOCTEXT("CrimeFactory", "EngineNotes", "Engine release notes"))
                    .OnNavigate_Lambda([URL = Data.Engine.ReleaseURL] { FPlatformProcess::LaunchURL(*URL, nullptr, nullptr); })];
        if (!Data.Engine.InstallerURL.IsEmpty())
            Body->AddSlot().AutoHeight()
                [SNew(SButton).Text(NSLOCTEXT("CrimeFactory", "GetEngineInstaller", "Get engine installer"))
                    .ToolTipText(NSLOCTEXT("CrimeFactory", "EngineInstallerHelp", "Opens the existing UE5-PD3 installer download page. Close editors before applying an engine patch. No engine binaries are changed by CrimeFactory."))
                    .OnClicked_Lambda([URL = Data.Engine.InstallerURL] { FPlatformProcess::LaunchURL(*URL, nullptr, nullptr); return FReply::Handled(); })];
    }
    return SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FStyleColors::Panel).Padding(24)
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [CrimeFactoryPackagingUI::MakeHeader(TEXT("PD3 Modkit"), NSLOCTEXT("CrimeFactory", "UpdateSection", "CRIMEFACTORY  /  UPDATES"),
            FText::FromString(TEXT("Modkit and engine updates")), NSLOCTEXT("CrimeFactory", "UpdateHelp", "Review each component separately. Engine patches use the UE5-PD3 installer."), Data.Severity)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 18)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0)
            [CrimeFactoryPackagingUI::MakeSummaryCard(NSLOCTEXT("CrimeFactory", "UpdateBehind", "Modkit commits to pull"), FText::AsNumber(Data.CommitsBehind), Data.Severity)]
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0)
            [CrimeFactoryPackagingUI::MakeSummaryCard(NSLOCTEXT("CrimeFactory", "UpdateChanges", "Local file changes"), FText::AsNumber(Data.LocalChanges), Data.Severity)]
            + SHorizontalBox::Slot().FillWidth(1)
            [CrimeFactoryPackagingUI::MakeSummaryCard(NSLOCTEXT("CrimeFactory", "UpdateAhead", "Local commits"), FText::AsNumber(Data.CommitsAhead), Data.Severity)]
        ]
        + SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox) + SScrollBox::Slot()[Body]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 18, 0, 0)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()[CrimeFactoryPackagingUI::MakeCopyReportButton(Data.BuildReport())]
            + SHorizontalBox::Slot().FillWidth(1)[SNullWidget::NullWidget]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
            [SNew(SButton).Text(Data.bCanUpdate ? NSLOCTEXT("CrimeFactory", "UpdateLater", "Later") : NSLOCTEXT("CrimeFactory", "UpdateClose", "Close"))
                .OnClicked_Lambda([OnClose] { OnClose.ExecuteIfBound(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).ButtonStyle(FAppStyle::Get(), "PrimaryButton").Text(NSLOCTEXT("CrimeFactory", "UpdateModkitNow", "Update modkit now"))
                .Visibility(Data.bCanUpdate ? EVisibility::Visible : EVisibility::Collapsed)
                .OnClicked_Lambda([OnUpdate, bAllowed = Data.bCanUpdate] { if (bAllowed) OnUpdate.ExecuteIfBound(); return FReply::Handled(); })]
        ]
    ];
}

bool CrimeFactoryUpdaterUI::CanPresent()
{
    if (IsRunningCommandlet() || FApp::IsUnattended() || !FSlateApplication::IsInitialized() || !FSlateApplication::Get().CanDisplayWindows()) return false;
    const FSlateApplication& Slate = FSlateApplication::Get();
    if (GIsSlowTask || Slate.IsTicking() || Slate.IsProcessingInput() || Slate.IsDragDropping() || Slate.GetActiveModalWindow().IsValid()) return false;
    const TSharedPtr<SWindow> RootWindow = FGlobalTabmanager::Get()->GetRootWindow();
    return RootWindow.IsValid() && RootWindow->GetNativeWindow().IsValid() && RootWindow->IsVisible()
        && !RootWindow->IsWindowMinimized() && !Slate.IsWindowInDestroyQueue(RootWindow.ToSharedRef());
}

bool CrimeFactoryUpdaterUI::Present(const FCrimeFactoryUpdatePresentation& Data)
{
    if (!CanPresent()) return false;
    const TSharedPtr<SWindow> RootWindow = FGlobalTabmanager::Get()->GetRootWindow();
    const auto Decision = MakeShared<bool>(false);
    const auto Window = SNew(SWindow).Title(NSLOCTEXT("CrimeFactory", "UpdaterTitle", "CrimeFactory · PD3 Modkit Updater"))
        .ClientSize(FVector2D(820, 700)).MinWidth(620).MinHeight(440).SupportsMaximize(true).SupportsMinimize(false);
    const TWeakPtr<SWindow> WeakWindow = Window;
    const auto Close = FSimpleDelegate::CreateLambda([WeakWindow] { if (const auto Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow(); });
    Window->SetContent(MakeContent(Data, FSimpleDelegate::CreateLambda([Decision, Close, bAllowed = Data.bCanUpdate]
    {
        *Decision = bAllowed;
        Close.ExecuteIfBound();
    }), Close));
    // Active windows can be transient menus or another asset editor being closed.
    // A native child is destroyed with its owner, including during initialization.
    FSlateApplication::Get().AddModalWindow(Window, RootWindow);
    return *Decision;
}
