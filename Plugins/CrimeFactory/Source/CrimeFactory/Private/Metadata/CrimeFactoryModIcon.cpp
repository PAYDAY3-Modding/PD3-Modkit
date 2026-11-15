#include "Metadata/CrimeFactoryModIcon.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

TSharedPtr<FSlateDynamicImageBrush> CrimeFactoryModIcon::Load(const FString& Filename)
{
    TArray<uint8> Compressed;
    FImage Decoded;
    if (!FFileHelper::LoadFileToArray(Compressed, *Filename) || !FImageUtils::DecompressImage(Compressed.GetData(), Compressed.Num(), Decoded)) return nullptr;
    if (Decoded.SizeX <= 0 || Decoded.SizeY <= 0 || Decoded.NumSlices != 1) return nullptr;
    Decoded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    TArray<uint8> Bytes;
    Bytes.Append(Decoded.RawData.GetData(), Decoded.RawData.Num());
    return FSlateDynamicImageBrush::CreateWithImageData(FName(*(TEXT("CrimeFactory.ModIcon.") + FGuid::NewGuid().ToString(EGuidFormats::Digits))),
        FVector2D(Decoded.SizeX, Decoded.SizeY), Bytes);
}

bool CrimeFactoryModIcon::IsValidPNG(const FString& Filename)
{
    if (!FPaths::GetExtension(Filename).Equals(TEXT("png"), ESearchCase::IgnoreCase)) return false;
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename)) return false;
    const uint8 Signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (Bytes.Num() < UE_ARRAY_COUNT(Signature) || FMemory::Memcmp(Bytes.GetData(), Signature, UE_ARRAY_COUNT(Signature)) != 0) return false;
    FImage Decoded;
    return FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Decoded)
        && Decoded.SizeX > 0 && Decoded.SizeY > 0 && Decoded.NumSlices == 1;
}

bool CrimeFactoryModIcon::CopyToResources(const FString& Filename, const FString& PluginDirectory, FText& Error)
{
    if (!IsValidPNG(Filename))
    {
        Error = NSLOCTEXT("CrimeFactoryIcon", "InvalidPNG", "Choose a valid PNG mod icon.");
        return false;
    }
    const FString Destination = PluginDirectory / TEXT("Resources/Icon128.png");
    if (FPaths::IsSamePath(Filename, Destination)) return true;
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true) ||
        IFileManager::Get().Copy(*Destination, *Filename, true, false) != COPY_OK)
    {
        Error = FText::Format(NSLOCTEXT("CrimeFactoryIcon", "CopyFailed", "Unable to save the mod icon to {0}. Check folder permissions."), FText::FromString(Destination));
        return false;
    }
    return true;
}
