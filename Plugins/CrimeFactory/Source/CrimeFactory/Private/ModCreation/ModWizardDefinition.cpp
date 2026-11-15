#include "ModCreation/ModWizardDefinition.h"
#include "Metadata/CrimeFactoryModIcon.h"
#include "Metadata/CrimeFactoryMetadataSync.h"

#include "Editor/CrimeFactory.h"
#include "Settings/CrimeFactorySettings.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Features/IPluginsEditorFeature.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Internationalization/Regex.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"   
#include "UObject/SavePackage.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "IAssetTools.h"
#include "AssetToolsModule.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Engine/PrimaryAssetLabel.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "UObject/UnrealType.h"
#include "UObject/StructOnScope.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/World.h"
#include "ObjectTools.h"
#include "PluginUtils.h"
#include "UObject/CoreRedirects.h"
#include "UObject/PackageFileSummary.h"

#define LOCTEXT_NAMESPACE "NewModWizard"


namespace
{
	static void CleanupTemplateRedirectors(const FString& PluginContentPath)
	{
		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		// Renaming and saving do not synchronously refresh the registry's disk data.
		// Reconcile deleted packages too, including when no redirector was needed.
		Registry.ScanPathsSynchronous({ PluginContentPath }, true);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*PluginContentPath), Assets, true);
		TArray<TStrongObjectPtr<UObjectRedirector>> Redirectors;
		TMap<FSoftObjectPath, FSoftObjectPath> RedirectMap;
		for (const FAssetData& Data : Assets)
		{
			if (!Data.IsRedirector()) continue;
			TArray<FString> Segments;
			Data.PackageName.ToString().ParseIntoArray(Segments, TEXT("/"), true);
			if (!Segments.ContainsByPredicate([](const FString& Segment)
				{ return (Segment.StartsWith(TEXT("PLACEHOLDER_")) && Segment.EndsWith(TEXT("MODNAME"))) || Segment.EndsWith(TEXT("_PLACEHOLDER_MODNAME")); })) continue;
			UObjectRedirector* Redirector = Cast<UObjectRedirector>(Data.GetAsset());
			UObject* Destination = Redirector ? Redirector->DestinationObject : nullptr;
			if (!Destination || Destination->IsA<UObjectRedirector>() ||
				!Destination->GetOutermost()->GetName().StartsWith(PluginContentPath) ||
				!FPackageName::DoesPackageExist(Destination->GetOutermost()->GetName())) continue;
			Redirectors.Emplace(Redirector);
			RedirectMap.Add(FSoftObjectPath(Redirector), FSoftObjectPath(Destination));
			if (Destination->IsA<UBlueprint>())
			{
				const FString OldPath = Redirector->GetPathName();
				const FString NewPath = Destination->GetPathName();
				RedirectMap.Add(FSoftObjectPath(OldPath + TEXT("_C")), FSoftObjectPath(NewPath + TEXT("_C")));
				RedirectMap.Add(
					FSoftObjectPath(Redirector->GetOutermost()->GetName() + TEXT(".Default__") + Redirector->GetName() + TEXT("_C")),
					FSoftObjectPath(Destination->GetOutermost()->GetName() + TEXT(".Default__") + Destination->GetName() + TEXT("_C")));
			}
		}
		if (Redirectors.IsEmpty()) return;

		// Limit automatic repair to this mod. Load hard references through the
		// redirectors, explicitly rewrite soft references, then persist both.
		TArray<UObject*> AssetsToSave;
		TArray<UPackage*> PackagesToFix;
		for (const FAssetData& Data : Assets)
		{
			if (Data.IsRedirector() || !FPackageName::DoesPackageExist(Data.PackageName.ToString())) continue;
			UObject* Asset = Data.GetAsset();
			if (!Asset)
			{
				UE_LOG(LogCrimeFactory, Error, TEXT("Cannot load %s; retaining template redirectors."), *Data.GetObjectPathString());
				return;
			}
			AssetsToSave.Add(Asset);
			PackagesToFix.AddUnique(Asset->GetOutermost());
		}
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get()
			.RenameReferencingSoftObjectPaths(PackagesToFix, RedirectMap);
		if (!UEditorAssetLibrary::SaveLoadedAssets(AssetsToSave, false))
		{
			UE_LOG(LogCrimeFactory, Error, TEXT("Could not save repaired template references under %s; retaining redirectors."), *PluginContentPath);
			return;
		}
		Registry.ScanPathsSynchronous({ PluginContentPath }, true);
		TArray<UObject*> UnreferencedRedirectors;
		for (const TStrongObjectPtr<UObjectRedirector>& Redirector : Redirectors)
		{
			TArray<FName> Referencers;
			Registry.GetReferencers(Redirector->GetOutermost()->GetFName(), Referencers,
				UE::AssetRegistry::EDependencyCategory::Package);
			if (Referencers.IsEmpty())
			{
				UnreferencedRedirectors.Add(Redirector.Get());
			}
			else
			{
				UE_LOG(LogCrimeFactory, Warning, TEXT("Retaining template redirector %s: %d package(s) still reference it."),
					*Redirector->GetPathName(), Referencers.Num());
			}
		}
		Redirectors.Reset();
		// Use Unreal's checked asset deletion so registry and package state are
		// updated together. Never delete .uasset files directly.
		if (!UnreferencedRedirectors.IsEmpty()) ObjectTools::DeleteObjects(UnreferencedRedirectors, false);
		Registry.ScanPathsSynchronous({ PluginContentPath }, true);
	}

	static bool EnsurePDMLDependencyInUPlugin(
		const FString& PluginDescriptorFileName,
		const FString& PluginName)
	{
		if (PluginName.Equals(TEXT("PDML"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		const FString PDMLPluginName = TEXT("PDML");
		FString PDMLSemVersion;

		if (const TSharedPtr<IPlugin> PDMLPlugin =
				IPluginManager::Get().FindPlugin(PDMLPluginName);
			PDMLPlugin.IsValid())
		{
			PDMLSemVersion = PDMLPlugin->GetDescriptor().VersionName;
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT("PDML was not found while creating mod %s. "
					 "The dependency will be written with an empty SemVersion."),
				*PluginName
			);
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *PluginDescriptorFileName))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT("Failed to read new mod descriptor: %s"),
				*PluginDescriptorFileName
			);
			return false;
		}

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(JsonText);

		if (!FJsonSerializer::Deserialize(Reader, RootObject) ||
			!RootObject.IsValid())
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT("Failed to parse new mod descriptor JSON: %s"),
				*PluginDescriptorFileName
			);
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> PluginDependencies;
		const TArray<TSharedPtr<FJsonValue>>* ExistingDependencies = nullptr;

		if (RootObject->TryGetArrayField(
				TEXT("Plugins"),
				ExistingDependencies) &&
			ExistingDependencies)
		{
			PluginDependencies = *ExistingDependencies;
		}

		TSharedPtr<FJsonObject> PDMLDependencyObject;

		for (const TSharedPtr<FJsonValue>& DependencyValue :
			 PluginDependencies)
		{
			if (!DependencyValue.IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* DependencyObjectPtr = nullptr;
			if (!DependencyValue->TryGetObject(DependencyObjectPtr) ||
				!DependencyObjectPtr ||
				!DependencyObjectPtr->IsValid())
			{
				continue;
			}

			FString DependencyName;
			if ((*DependencyObjectPtr)->TryGetStringField(
					TEXT("Name"),
					DependencyName) &&
				DependencyName.Equals(
					PDMLPluginName,
					ESearchCase::IgnoreCase))
			{
				PDMLDependencyObject = *DependencyObjectPtr;
				break;
			}
		}

		if (!PDMLDependencyObject.IsValid())
		{
			PDMLDependencyObject = MakeShared<FJsonObject>();
			PluginDependencies.Add(
				MakeShared<FJsonValueObject>(PDMLDependencyObject)
			);
		}

		PDMLDependencyObject->SetStringField(
			TEXT("Name"),
			PDMLPluginName
		);
		PDMLDependencyObject->SetBoolField(
			TEXT("Enabled"),
			true
		);
		PDMLDependencyObject->SetStringField(
			TEXT("SemVersion"),
			PDMLSemVersion
		);

		// Make PDML a hard dependency if an old optional entry was present.
		PDMLDependencyObject->RemoveField(TEXT("Optional"));

		RootObject->SetArrayField(
			TEXT("Plugins"),
			PluginDependencies
		);

		FString UpdatedJson;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&UpdatedJson);

		if (!FJsonSerializer::Serialize(
				RootObject.ToSharedRef(),
				Writer))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT("Failed to serialize new mod descriptor JSON: %s"),
				*PluginDescriptorFileName
			);
			return false;
		}

		if (!FFileHelper::SaveStringToFile(
				UpdatedJson,
				*PluginDescriptorFileName,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT("Failed to save PDML dependency to: %s"),
				*PluginDescriptorFileName
			);
			return false;
		}

		UE_LOG(
			LogCrimeFactory,
			Display,
			TEXT("Wrote PDML dependency to %s (SemVersion=%s)"),
			*PluginDescriptorFileName,
			*PDMLSemVersion
		);

		return true;
	}


	static FString NormalizeMetadataPropertyKey(const FString& InValue)
	{
		FString Result;
		Result.Reserve(InValue.Len());

		for (const TCHAR Character : InValue)
		{
			if (FChar::IsAlnum(Character))
			{
				Result.AppendChar(
					FChar::ToLower(Character)
				);
			}
		}

		return Result;
	}

	static FProperty* FindMetadataProperty(
		UStruct* Struct,
		std::initializer_list<const TCHAR*> Aliases)
	{
		if (!Struct)
		{
			return nullptr;
		}

		TArray<FString> NormalizedAliases;

		for (const TCHAR* Alias : Aliases)
		{
			NormalizedAliases.Add(
				NormalizeMetadataPropertyKey(Alias)
			);
		}

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Property = *It;

			const FString PropertyName =
				NormalizeMetadataPropertyKey(
					Property->GetName()
				);

			const FString DisplayName =
				NormalizeMetadataPropertyKey(
					Property->GetDisplayNameText().ToString()
				);

			for (const FString& Alias : NormalizedAliases)
			{
				if (PropertyName == Alias ||
					DisplayName == Alias)
				{
					return Property;
				}
			}
		}

		return nullptr;
	}

	static bool SetMetadataTextValue(
		UObject* Object,
		FProperty* Property,
		const FString& Value)
	{
		if (!Object || !Property)
		{
			return false;
		}

		void* ValuePtr =
			Property->ContainerPtrToValuePtr<void>(Object);

		if (FStrProperty* StringProperty =
				CastField<FStrProperty>(Property))
		{
			StringProperty->SetPropertyValue(
				ValuePtr,
				Value
			);
			return true;
		}

		if (FNameProperty* NameProperty =
				CastField<FNameProperty>(Property))
		{
			NameProperty->SetPropertyValue(
				ValuePtr,
				Value.IsEmpty()
					? NAME_None
					: FName(*Value)
			);
			return true;
		}

		if (FTextProperty* TextProperty =
				CastField<FTextProperty>(Property))
		{
			TextProperty->SetPropertyValue(
				ValuePtr,
				FText::FromString(Value)
			);
			return true;
		}

		return false;
	}

	static bool SetMetadataStringArray(
		UObject* Object,
		FProperty* Property,
		const TArray<FString>& Values)
	{
		if (!Object || !Property)
		{
			return false;
		}

		FArrayProperty* ArrayProperty =
			CastField<FArrayProperty>(Property);

		if (!ArrayProperty)
		{
			return false;
		}

		FScriptArrayHelper ArrayHelper(
			ArrayProperty,
			ArrayProperty->ContainerPtrToValuePtr<void>(Object)
		);

		ArrayHelper.EmptyValues();

		for (const FString& Value : Values)
		{
			if (Value.IsEmpty())
			{
				continue;
			}

			const int32 Index =
				ArrayHelper.AddValue();

			void* ElementPtr =
				ArrayHelper.GetRawPtr(Index);

			if (FStrProperty* StringProperty =
					CastField<FStrProperty>(
						ArrayProperty->Inner))
			{
				StringProperty->SetPropertyValue(
					ElementPtr,
					Value
				);
			}
			else if (FNameProperty* NameProperty =
					 CastField<FNameProperty>(
						 ArrayProperty->Inner))
			{
				NameProperty->SetPropertyValue(
					ElementPtr,
					FName(*Value)
				);
			}
			else if (FTextProperty* TextProperty =
					 CastField<FTextProperty>(
						 ArrayProperty->Inner))
			{
				TextProperty->SetPropertyValue(
					ElementPtr,
					FText::FromString(Value)
				);
			}
			else
			{
				ArrayHelper.RemoveValues(Index, 1);
				return false;
			}
		}

		return true;
	}

	static bool FindEnumValueByName(
		UEnum* Enum,
		const FString& RequestedValue,
		int64& OutValue)
	{
		if (!Enum)
		{
			return false;
		}

		const FString RequestedKey =
			NormalizeMetadataPropertyKey(RequestedValue);

		for (int32 Index = 0;
			 Index < Enum->NumEnums();
			 ++Index)
		{
			const FString EnumName =
				Enum->GetNameStringByIndex(Index);

			if (EnumName.EndsWith(TEXT("_MAX")))
			{
				continue;
			}

			const FString NameKey =
				NormalizeMetadataPropertyKey(EnumName);

			const FString DisplayKey =
				NormalizeMetadataPropertyKey(
					Enum->GetDisplayNameTextByIndex(Index)
						.ToString()
				);

			if (NameKey == RequestedKey ||
				DisplayKey == RequestedKey ||
				NameKey.EndsWith(RequestedKey) ||
				DisplayKey.EndsWith(RequestedKey))
			{
				OutValue =
					Enum->GetValueByIndex(Index);

				return true;
			}
		}

		return false;
	}

	static bool SetMetadataEnumValue(
		UObject* Object,
		FProperty* Property,
		const FString& RequestedValue)
	{
		if (!Object || !Property)
		{
			return false;
		}

		if (FEnumProperty* EnumProperty =
				CastField<FEnumProperty>(Property))
		{
			int64 EnumValue = 0;

			if (!FindEnumValueByName(
					EnumProperty->GetEnum(),
					RequestedValue,
					EnumValue))
			{
				return false;
			}

			void* ValuePtr =
				EnumProperty->ContainerPtrToValuePtr<void>(
					Object
				);

			EnumProperty->GetUnderlyingProperty()
				->SetIntPropertyValue(
					ValuePtr,
					EnumValue
				);

			return true;
		}

		if (FByteProperty* ByteProperty =
				CastField<FByteProperty>(Property))
		{
			if (!ByteProperty->Enum)
			{
				return false;
			}

			int64 EnumValue = 0;

			if (!FindEnumValueByName(
					ByteProperty->Enum,
					RequestedValue,
					EnumValue))
			{
				return false;
			}

			ByteProperty->SetPropertyValue_InContainer(
				Object,
				static_cast<uint8>(EnumValue)
			);

			return true;
		}

		return false;
	}

	static bool IsGeneratedModActorClass(
		UClass* GeneratedClass,
		UClass* ModActorBaseClass)
	{
		if (!GeneratedClass)
		{
			return false;
		}

		if (ModActorBaseClass &&
			GeneratedClass != ModActorBaseClass &&
			GeneratedClass->IsChildOf(ModActorBaseClass))
		{
			return true;
		}

		// Fallback if the PDML base class could not be loaded directly.
		for (UClass* Class = GeneratedClass;
			 Class;
			 Class = Class->GetSuperClass())
		{
			if (Class->GetName().Equals(
					TEXT("ModActorBase_C"),
					ESearchCase::IgnoreCase))
			{
				return GeneratedClass != Class;
			}
		}

		return false;
	}

	static TArray<UClass*> FindModActorClassesInPlugin(
		const FString& PluginContentPath)
	{
		TArray<UClass*> Result;

		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry")
			);

		IAssetRegistry& AssetRegistry =
			AssetRegistryModule.Get();

		TArray<FString> PathsToScan;
		PathsToScan.Add(PluginContentPath);

		AssetRegistry.ScanPathsSynchronous(
			PathsToScan,
			true
		);

		TArray<FAssetData> Assets;

		AssetRegistry.GetAssetsByPath(
			FName(*PluginContentPath),
			Assets,
			true
		);

		UClass* ModActorBaseClass =
			LoadObject<UClass>(
				nullptr,
				TEXT(
					"/PDML/Blueprints/ModActorBase."
					"ModActorBase_C"
				)
			);

		for (const FAssetData& AssetData : Assets)
		{
			UBlueprint* Blueprint =
				Cast<UBlueprint>(AssetData.GetAsset());

			if (!Blueprint ||
				!Blueprint->GeneratedClass)
			{
				continue;
			}

			if (IsGeneratedModActorClass(
					Blueprint->GeneratedClass,
					ModActorBaseClass))
			{
				Result.AddUnique(
					Blueprint->GeneratedClass
				);
			}
		}

		Result.Sort(
			[](const UClass& A, const UClass& B)
			{
				return A.GetPathName() <
					B.GetPathName();
			}
		);

		return Result;
	}

	static FProperty* FindModActorClassField(
		UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return nullptr;
		}

		FProperty* FallbackClassProperty = nullptr;

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Property = *It;

			const FString PropertyKey =
				NormalizeMetadataPropertyKey(
					Property->GetDisplayNameText().ToString()
				);

			const bool bClassCompatible =
				CastField<FClassProperty>(Property) != nullptr ||
				CastField<FSoftClassProperty>(Property) != nullptr;

			if (!bClassCompatible)
			{
				continue;
			}

			if (!FallbackClassProperty)
			{
				FallbackClassProperty = Property;
			}

			if (PropertyKey.Contains(
					TEXT("modactorclass")))
			{
				return Property;
			}
		}

		return FallbackClassProperty;
	}

	static bool SetStructClassValue(
		FProperty* Property,
		void* StructMemory,
		UClass* ClassValue)
	{
		if (!Property ||
			!StructMemory ||
			!ClassValue)
		{
			return false;
		}

		if (FClassProperty* ClassProperty =
				CastField<FClassProperty>(Property))
		{
			void* ValuePtr =
				ClassProperty->ContainerPtrToValuePtr<void>(
					StructMemory
				);

			ClassProperty->SetObjectPropertyValue(
				ValuePtr,
				ClassValue
			);

			return true;
		}

		if (FSoftClassProperty* SoftClassProperty =
				CastField<FSoftClassProperty>(Property))
		{
			void* ValuePtr =
				SoftClassProperty->ContainerPtrToValuePtr<void>(
					StructMemory
				);

			SoftClassProperty->SetPropertyValue(
				ValuePtr,
				FSoftObjectPtr(ClassValue)
			);

			return true;
		}

		return false;
	}

	static bool PopulateModActorsArray(
		UObject* DataAsset,
		FProperty* Property,
		const TArray<UClass*>& ModActorClasses)
	{
		if (!DataAsset || !Property)
		{
			return false;
		}

		FArrayProperty* ArrayProperty =
			CastField<FArrayProperty>(Property);

		if (!ArrayProperty)
		{
			return false;
		}

		FStructProperty* StructProperty =
			CastField<FStructProperty>(
				ArrayProperty->Inner
			);

		if (!StructProperty ||
			!StructProperty->Struct)
		{
			return false;
		}

		FProperty* ActorClassProperty =
			FindModActorClassField(
				StructProperty->Struct
			);

		if (!ActorClassProperty)
		{
			return false;
		}

		FScriptArrayHelper ArrayHelper(
			ArrayProperty,
			ArrayProperty->ContainerPtrToValuePtr<void>(
				DataAsset
			)
		);

		/*
		 * The LogicMod template already contains one ModActors element
		 * with useful defaults (Spawn Immediately, respawn, singleton).
		 * Preserve that struct as the prototype for every auto-discovered
		 * actor instead of constructing zeroed entries.
		 */
		FStructOnScope Prototype(
			StructProperty->Struct
		);

		const bool bHasPrototype =
			ArrayHelper.Num() > 0;

		if (bHasPrototype)
		{
			StructProperty->Struct->CopyScriptStruct(
				Prototype.GetStructMemory(),
				ArrayHelper.GetRawPtr(0)
			);
		}

		ArrayHelper.EmptyValues();

		for (UClass* ModActorClass : ModActorClasses)
		{
			if (!ModActorClass)
			{
				continue;
			}

			const int32 NewIndex =
				ArrayHelper.AddValue();

			void* ElementMemory =
				ArrayHelper.GetRawPtr(NewIndex);

			if (bHasPrototype)
			{
				StructProperty->Struct->CopyScriptStruct(
					ElementMemory,
					Prototype.GetStructMemory()
				);
			}

			if (!SetStructClassValue(
					ActorClassProperty,
					ElementMemory,
					ModActorClass))
			{
				ArrayHelper.RemoveValues(
					NewIndex,
					1
				);

				return false;
			}
		}

		return true;
	}


	static bool IsClassOrChildOfNamedClass(
		UClass* CandidateClass,
		const FString& RequestedClassName)
	{
		if (!CandidateClass)
		{
			return false;
		}

		const FString RequestedKey =
			NormalizeMetadataPropertyKey(RequestedClassName);

		for (UClass* Class = CandidateClass;
			 Class;
			 Class = Class->GetSuperClass())
		{
			if (NormalizeMetadataPropertyKey(Class->GetName()) == RequestedKey)
			{
				return true;
			}
		}

		return false;
	}

	static TArray<UObject*> FindHeistDataAssetsInPlugin(
		const FString& PluginContentPath)
	{
		TArray<UObject*> Result;

		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry")
			);

		IAssetRegistry& AssetRegistry =
			AssetRegistryModule.Get();

		TArray<FString> PathsToScan;
		PathsToScan.Add(PluginContentPath);

		AssetRegistry.ScanPathsSynchronous(
			PathsToScan,
			true
		);

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPath(
			FName(*PluginContentPath),
			Assets,
			true
		);

		UClass* HeistDataAssetClass =
			FindObject<UClass>(
				nullptr,
				TEXT("/Script/Starbreeze.PD3HeistDataAsset")
			);

		for (const FAssetData& AssetData : Assets)
		{
			UObject* Asset = AssetData.GetAsset();

			if (!Asset)
			{
				continue;
			}

			const bool bIsHeistDataAsset =
				HeistDataAssetClass
					? Asset->IsA(HeistDataAssetClass)
					: IsClassOrChildOfNamedClass(
						Asset->GetClass(),
						TEXT("PD3HeistDataAsset")
					);

			if (bIsHeistDataAsset)
			{
				Result.AddUnique(Asset);
			}
		}

		Result.Sort(
			[](const UObject& A, const UObject& B)
			{
				return A.GetPathName() < B.GetPathName();
			}
		);

		return Result;
	}

	static bool PopulateObjectArray(
		UObject* Owner,
		FProperty* Property,
		const TArray<UObject*>& Values)
	{
		if (!Owner || !Property)
		{
			return false;
		}

		FArrayProperty* ArrayProperty =
			CastField<FArrayProperty>(Property);

		if (!ArrayProperty)
		{
			return false;
		}

		FSoftObjectProperty* SoftObjectProperty =
			CastField<FSoftObjectProperty>(
				ArrayProperty->Inner
			);

		FObjectPropertyBase* ObjectProperty =
			SoftObjectProperty
				? nullptr
				: CastField<FObjectPropertyBase>(
					ArrayProperty->Inner
				);

		if (!SoftObjectProperty && !ObjectProperty)
		{
			return false;
		}

		FScriptArrayHelper ArrayHelper(
			ArrayProperty,
			ArrayProperty->ContainerPtrToValuePtr<void>(
				Owner
			)
		);

		ArrayHelper.EmptyValues();

		for (UObject* Value : Values)
		{
			if (!Value)
			{
				continue;
			}

			UClass* ExpectedClass =
				SoftObjectProperty
					? SoftObjectProperty->PropertyClass
					: ObjectProperty->PropertyClass;

			if (ExpectedClass && !Value->IsA(ExpectedClass))
			{
				UE_LOG(
					LogCrimeFactory,
					Warning,
					TEXT(
						"Skipping %s while populating %s: "
						"asset is not compatible with %s."
					),
					*Value->GetPathName(),
					*Property->GetName(),
					*ExpectedClass->GetName()
				);
				continue;
			}

			const int32 Index = ArrayHelper.AddValue();
			void* ElementPtr = ArrayHelper.GetRawPtr(Index);

			if (SoftObjectProperty)
			{
				FSoftObjectPtr* ObjectPtr =
					static_cast<FSoftObjectPtr*>(ElementPtr);
				*ObjectPtr = FSoftObjectPath(Value);
			}
			else
			{
				ObjectProperty->SetObjectPropertyValue(
					ElementPtr,
					Value
				);
			}
		}

		return true;
	}

	static bool FindTemplateLevelInPlugin(
		const FString& PluginContentPath,
		const FString& PluginName,
		FAssetData& OutLevelAsset)
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry")
			);

		IAssetRegistry& AssetRegistry =
			AssetRegistryModule.Get();

		TArray<FString> PathsToScan;
		PathsToScan.Add(PluginContentPath);

		AssetRegistry.ScanPathsSynchronous(
			PathsToScan,
			true
		);

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPath(
			FName(*PluginContentPath),
			Assets,
			true
		);

		const FString RenamedLevelName =
			FString::Printf(TEXT("LVL_%s"), *PluginName);

		const FString PlaceholderLevelName =
			TEXT("PLACEHOLDER_LVL_MODNAME");

		TArray<FAssetData> WorldAssets;

		for (const FAssetData& AssetData : Assets)
		{
			const bool bIsWorld =
				AssetData.AssetClassPath ==
					UWorld::StaticClass()->GetClassPathName();

			if (!bIsWorld)
			{
				continue;
			}

			const FString AssetName =
				AssetData.AssetName.ToString();

			if (AssetName.Equals(
					RenamedLevelName,
					ESearchCase::IgnoreCase))
			{
				OutLevelAsset = AssetData;
				return true;
			}

			if (AssetName.Equals(
					PlaceholderLevelName,
					ESearchCase::IgnoreCase))
			{
				OutLevelAsset = AssetData;
				return true;
			}

			WorldAssets.Add(AssetData);
		}

		// A custom-heist template normally contains exactly one level. If the
		// expected placeholder/final name was changed manually, still use the
		// only world rather than leaving LevelPath unset.
		if (WorldAssets.Num() == 1)
		{
			OutLevelAsset = WorldAssets[0];
			return true;
		}

		return false;
	}

	static bool SetAssetReferenceValue(
		UObject* Owner,
		FProperty* Property,
		UObject* Asset)
	{
		if (!Owner || !Property || !Asset)
		{
			return false;
		}

		void* ValuePtr =
			Property->ContainerPtrToValuePtr<void>(Owner);

		if (FSoftObjectProperty* SoftObjectProperty =
				CastField<FSoftObjectProperty>(Property))
		{
			if (SoftObjectProperty->PropertyClass &&
				!Asset->IsA(SoftObjectProperty->PropertyClass))
			{
				return false;
			}

			FSoftObjectPtr* ObjectPtr =
				static_cast<FSoftObjectPtr*>(ValuePtr);
			*ObjectPtr = FSoftObjectPath(Asset);
			return true;
		}

		if (FObjectPropertyBase* ObjectProperty =
				CastField<FObjectPropertyBase>(Property))
		{
			if (ObjectProperty->PropertyClass &&
				!Asset->IsA(ObjectProperty->PropertyClass))
			{
				return false;
			}

			ObjectProperty->SetObjectPropertyValue(
				ValuePtr,
				Asset
			);
			return true;
		}

		if (FStructProperty* StructProperty =
				CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct &&
				StructProperty->Struct->GetName().Equals(
					TEXT("SoftObjectPath"),
					ESearchCase::IgnoreCase))
			{
				FSoftObjectPath* SoftPath =
					reinterpret_cast<FSoftObjectPath*>(
						ValuePtr
					);

				*SoftPath = FSoftObjectPath(Asset);
				return true;
			}
		}

		return SetMetadataTextValue(
			Owner,
			Property,
			FSoftObjectPath(Asset).ToString()
		);
	}

	static int32 PopulateHeistData(
		UObject* ModDataAsset,
		UClass* ModDataClass,
		const FString& PluginContentPath,
		const FString& PluginName)
	{
		if (!ModDataAsset || !ModDataClass)
		{
			return 0;
		}

		const TArray<UObject*> HeistDataAssets =
			FindHeistDataAssetsInPlugin(PluginContentPath);

		FProperty* HeistProperty =
			FindMetadataProperty(
				ModDataClass,
				{
					TEXT("Heist"),
					TEXT("Heists")
				}
			);

		int32 FieldsUpdated = 0;

		if (HeistProperty)
		{
			if (PopulateObjectArray(
					ModDataAsset,
					HeistProperty,
					HeistDataAssets))
			{
				++FieldsUpdated;

				UE_LOG(
					LogCrimeFactory,
					Display,
					TEXT(
						"%s: populated Heists with %d "
						"PD3HeistDataAsset asset(s)."
					),
					*PluginName,
					HeistDataAssets.Num()
				);
			}
			else
			{
				UE_LOG(
					LogCrimeFactory,
					Warning,
					TEXT(
						"%s: found the Heist/Heists property, "
						"but it is not a compatible object array."
					),
					*PluginName
				);
			}
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Verbose,
				TEXT("%s: Data Asset has no Heist/Heists property."),
				*PluginName
			);
		}

		if (HeistDataAssets.Num() == 0)
		{
			return FieldsUpdated;
		}

		FAssetData LevelAssetData;
		if (!FindTemplateLevelInPlugin(
				PluginContentPath,
				PluginName,
				LevelAssetData))
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"%s: found %d heist Data Asset(s), but no "
					"LVL_%s / PLACEHOLDER_LVL_MODNAME level was found."
				),
				*PluginName,
				HeistDataAssets.Num(),
				*PluginName
			);
			return FieldsUpdated;
		}

		UObject* LevelAsset = LevelAssetData.GetAsset();
		if (!LevelAsset)
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"%s: failed to load level %s for LevelPath population."
				),
				*PluginName,
				*LevelAssetData.GetObjectPathString()
			);
			return FieldsUpdated;
		}

		UEditorAssetSubsystem* AssetSubsystem =
			GEditor
				? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
				: nullptr;

		for (UObject* HeistDataAsset : HeistDataAssets)
		{
			if (!HeistDataAsset)
			{
				continue;
			}

			FProperty* LevelPathProperty =
				FindMetadataProperty(
					HeistDataAsset->GetClass(),
					{
						TEXT("LevelPath"),
						TEXT("Level Path")
					}
				);

			if (!LevelPathProperty ||
				!SetAssetReferenceValue(
					HeistDataAsset,
					LevelPathProperty,
					LevelAsset))
			{
				UE_LOG(
					LogCrimeFactory,
					Warning,
					TEXT(
						"%s: could not set LevelPath on %s."
					),
					*PluginName,
					*HeistDataAsset->GetPathName()
				);
				continue;
			}

			HeistDataAsset->MarkPackageDirty();

			if (AssetSubsystem)
			{
				const FString HeistPackagePath =
					HeistDataAsset->GetOutermost()->GetName();

				if (!AssetSubsystem->SaveAsset(HeistPackagePath))
				{
					UE_LOG(
						LogCrimeFactory,
						Warning,
						TEXT("%s: failed to save heist Data Asset %s."),
						*PluginName,
						*HeistPackagePath
					);
				}
			}

			UE_LOG(
				LogCrimeFactory,
				Display,
				TEXT("%s: set %s LevelPath to %s."),
				*PluginName,
				*HeistDataAsset->GetName(),
				*LevelAsset->GetPathName()
			);
		}

		return FieldsUpdated;
	}

	static bool PopulateGeneratedModDataAsset(
		const FString& PluginName)
	{
		TSharedPtr<IPlugin> Plugin =
			IPluginManager::Get().FindPlugin(PluginName);

		if (!Plugin.IsValid())
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"Unable to populate mod metadata: "
					"plugin %s was not found."
				),
				*PluginName
			);

			return false;
		}

		FString PluginContentPath =
			Plugin->GetMountedAssetPath();

		if (!PluginContentPath.EndsWith(TEXT("/")))
		{
			PluginContentPath += TEXT("/");
		}

		const FString DataAssetPackagePath =
			PluginContentPath +
			FString::Printf(
				TEXT("DA_%s"),
				*PluginName
			);

		const FString DataAssetObjectPath =
			DataAssetPackagePath +
			TEXT(".") +
			FString::Printf(
				TEXT("DA_%s"),
				*PluginName
			);

		UObject* DataAsset =
			FSoftObjectPath(
				DataAssetObjectPath
			).TryLoad();

		if (!DataAsset)
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"No generated mod metadata Data Asset "
					"was found at %s. Skipping auto-population."
				),
				*DataAssetObjectPath
			);

			return false;
		}

		const FPluginDescriptor& Descriptor =
			Plugin->GetDescriptor();

		const FString FriendlyModName =
			Descriptor.FriendlyName.IsEmpty()
				? PluginName
				: Descriptor.FriendlyName;

		const FString ModVersion =
			Descriptor.VersionName.IsEmpty()
				? TEXT("1.0.0")
				: Descriptor.VersionName;

		const FString ModDescription =
			Descriptor.Description;

		TArray<FString> Authors;
		const TSharedRef<FJsonObject> MetadataJson = MakeShared<FJsonObject>();
		if (Descriptor.CachedJson) FJsonObject::Duplicate(Descriptor.CachedJson, MetadataJson);
		Descriptor.UpdateJson(MetadataJson.Get());
		if (!MetadataJson->TryGetStringArrayField(TEXT("ModAuthors"), Authors) && !Descriptor.CreatedBy.IsEmpty())
			Authors.Add(Descriptor.CreatedBy);

		FString PDMLVersion;

		if (const TSharedPtr<IPlugin> PDMLPlugin =
				IPluginManager::Get().FindPlugin(TEXT("PDML"));
			PDMLPlugin.IsValid())
		{
			PDMLVersion =
				PDMLPlugin->GetDescriptor().VersionName;
		}

		UClass* DataClass =
			DataAsset->GetClass();

		int32 FieldsUpdated = 0;

		if (SetMetadataTextValue(
				DataAsset,
				FindMetadataProperty(
					DataClass,
					{ TEXT("ModID"), TEXT("Mod ID") }
				),
				PluginName))
		{
			++FieldsUpdated;
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"%s: could not set 'Mod ID' on %s."
				),
				*PluginName,
				*DataAssetObjectPath
			);
		}

		if (SetMetadataTextValue(
				DataAsset,
				FindMetadataProperty(
					DataClass,
					{
						TEXT("ModVersion"),
						TEXT("Mod Version")
					}
				),
				ModVersion))
		{
			++FieldsUpdated;
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"%s: could not set 'Mod Version' on %s."
				),
				*PluginName,
				*DataAssetObjectPath
			);
		}

		if (SetMetadataEnumValue(
				DataAsset,
				FindMetadataProperty(
					DataClass,
					{
						TEXT("ModEnvironment"),
						TEXT("Mod Environment")
					}
				),
				TEXT("Client")))
		{
			++FieldsUpdated;
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"%s: could not set Mod Environment to Client."
				),
				*PluginName
			);
		}

		if (SetMetadataTextValue(
				DataAsset,
				FindMetadataProperty(
					DataClass,
					{ TEXT("ModName"), TEXT("Mod Name") }
				),
				FriendlyModName))
		{
			++FieldsUpdated;
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"%s: could not set 'Mod Name'."
				),
				*PluginName
			);
		}

		if (SetMetadataStringArray(
				DataAsset,
				FindMetadataProperty(
					DataClass,
					{
						TEXT("ModAuthors"),
						TEXT("Mod Authors")
					}
				),
				Authors))
		{
			++FieldsUpdated;
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"%s: could not set 'Mod Authors'."
				),
				*PluginName
			);
		}

		if (SetMetadataTextValue(
				DataAsset,
				FindMetadataProperty(
					DataClass,
					{
						TEXT("ModDescription"),
						TEXT("Mod Description")
					}
				),
				ModDescription))
		{
			++FieldsUpdated;
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"%s: could not set 'Mod Description'."
				),
				*PluginName
			);
		}

		if (!PDMLVersion.IsEmpty())
		{
			if (SetMetadataTextValue(
					DataAsset,
					FindMetadataProperty(
						DataClass,
						{
							TEXT("ModLoaderVersion"),
							TEXT("Mod Loader Version")
						}
					),
					PDMLVersion))
			{
				++FieldsUpdated;
			}
			else
			{
				UE_LOG(
					LogCrimeFactory,
					Warning,
					TEXT(
						"%s: could not set 'Mod Loader Version'."
					),
					*PluginName
				);
			}
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"%s: PDML VersionName was unavailable; "
					"Mod Loader Version was left unchanged."
				),
				*PluginName
			);
		}

		const TArray<UClass*> ModActorClasses =
			FindModActorClassesInPlugin(
				PluginContentPath
			);

		FProperty* ModActorsProperty =
			FindMetadataProperty(
				DataClass,
				{ TEXT("ModActors"), TEXT("Mod Actors") }
			);

		if (ModActorsProperty)
		{
			if (PopulateModActorsArray(
					DataAsset,
					ModActorsProperty,
					ModActorClasses))
			{
				++FieldsUpdated;

				UE_LOG(
					LogCrimeFactory,
					Display,
					TEXT(
						"%s: populated Mod Actors with "
						"%d ModActorBase-derived Blueprint class(es)."
					),
					*PluginName,
					ModActorClasses.Num()
				);
			}
			else
			{
				UE_LOG(
					LogCrimeFactory,
					Warning,
					TEXT(
						"%s: found the Mod Actors property, "
						"but could not populate its struct array."
					),
					*PluginName
				);
			}
		}
		else
		{
			UE_LOG(
				LogCrimeFactory,
				Verbose,
				TEXT(
					"%s: Data Asset has no Mod Actors property."
				),
				*PluginName
			);
		}

		FieldsUpdated += PopulateHeistData(
			DataAsset,
			DataClass,
			PluginContentPath,
			PluginName
		);

		DataAsset->MarkPackageDirty();

		UEditorAssetSubsystem* AssetSubsystem =
			GEditor
				? GEditor->GetEditorSubsystem<
					UEditorAssetSubsystem>()
				: nullptr;

		if (!AssetSubsystem)
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"%s: EditorAssetSubsystem was unavailable; "
					"generated metadata was not saved."
				),
				*PluginName
			);

			return false;
		}

		if (!AssetSubsystem->SaveAsset(
				DataAssetPackagePath))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"%s: failed to save populated Data Asset %s."
				),
				*PluginName,
				*DataAssetPackagePath
			);

			return false;
		}

		UE_LOG(
			LogCrimeFactory,
			Display,
			TEXT(
				"%s: auto-populated generated mod metadata "
				"Data Asset (%d fields/groups updated): %s"
			),
			*PluginName,
			FieldsUpdated,
			*DataAssetObjectPath
		);

		return true;
	}

	class FCrimeFactoryPluginTemplateDescription final : public FPluginTemplateDescription
	{
	public:
		TArray<FString> RequiredPluginNames;
		FCrimeFactoryPluginTemplateDescription(
			const FText& InName,
			const FText& InDescription,
			const FString& InOnDiskPath,
			bool bInCanContainContent,
			EHostType::Type InModuleDescriptorType)
			: FPluginTemplateDescription(
				InName,
				InDescription,
				InOnDiskPath,
				bInCanContainContent,
				InModuleDescriptorType)
		{
		}

		virtual void CustomizeDescriptorBeforeCreation(FPluginDescriptor& Descriptor) override
		{
			FPluginTemplateDescription::CustomizeDescriptorBeforeCreation(Descriptor);
			for (const FString& RequiredName : RequiredPluginNames)
			{
				FPluginReferenceDescriptor Reference(RequiredName, true);
				const auto RequiredPlugin = IPluginManager::Get().FindPlugin(RequiredName);
				if (RequiredPlugin && !RequiredPlugin->GetDescriptor().VersionName.IsEmpty())
					Reference.AdditionalFieldsToWrite.Add(TEXT("SemVersion"), MakeShared<FJsonValueString>(TEXT("^") + RequiredPlugin->GetDescriptor().VersionName));
				Descriptor.Plugins.Add(Reference);
			}

			const FString PDMLPluginName = TEXT("PDML");
			FString PDMLSemVersion;

			const TSharedPtr<IPlugin> PDMLPlugin =
				IPluginManager::Get().FindPlugin(PDMLPluginName);

			if (PDMLPlugin.IsValid())
			{
				PDMLSemVersion = PDMLPlugin->GetDescriptor().VersionName;
			}
			else
			{
				UE_LOG(
					LogCrimeFactory,
					Warning,
					TEXT("PDML was not found while creating a mod. "
						 "The dependency will be created with an empty SemVersion.")
				);
			}

			FPluginReferenceDescriptor* ExistingPDML =
				Descriptor.Plugins.FindByPredicate(
					[&PDMLPluginName](const FPluginReferenceDescriptor& Reference)
					{
						return Reference.Name.Equals(
							PDMLPluginName,
							ESearchCase::IgnoreCase
						);
					}
				);

			if (ExistingPDML)
			{
				ExistingPDML->bEnabled = true;
				ExistingPDML->bOptional = false;
				ExistingPDML->AdditionalFieldsToWrite.Add(
					TEXT("SemVersion"),
					MakeShared<FJsonValueString>(PDMLSemVersion)
				);
			}
			else
			{
				FPluginReferenceDescriptor PDMLDependency(
					PDMLPluginName,
					true
				);

				PDMLDependency.bOptional = false;
				PDMLDependency.AdditionalFieldsToWrite.Add(
					TEXT("SemVersion"),
					MakeShared<FJsonValueString>(PDMLSemVersion)
				);

				Descriptor.Plugins.Add(PDMLDependency);
			}

			UE_LOG(
				LogCrimeFactory,
				Display,
				TEXT("Pre-creation descriptor: added PDML dependency (SemVersion=%s)"),
				*PDMLSemVersion
			);
		}
	};
}

FModWizardDefinition::FModWizardDefinition()
{
	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("CrimeFactory"));
	if (Plugin) PluginBaseDir = Plugin->GetBaseDir();
	RefreshTemplates();
}

void FModWizardDefinition::RefreshTemplates()
{
	const FString Selected = GetSelectedTemplate() ? GetSelectedTemplate()->OnDiskPath : FString();
	ClearTemplateSelection();
	ModTemplateDefinitions.Reset();
	TemplateDefinitions.Reset();
	DiscoveryError = FText::GetEmpty();
	TArray<FString> Folders;
	if (PluginBaseDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*GetTemplatesDirectory()))
	{
		DiscoveryError = LOCTEXT("TemplatesUnavailable", "The Templates directory is unavailable. Check the CrimeFactory installation and folder permissions.");
		return;
	}
	if (!IFileManager::Get().IterateDirectory(*GetTemplatesDirectory(), [&Folders](const TCHAR* Path, bool bDirectory)
		{ if (bDirectory) Folders.Add(FString(Path)); return true; }))
	{
		DiscoveryError = LOCTEXT("TemplatesUnreadable", "Unable to read the Templates directory. Check folder permissions.");
		return;
	}
	Folders.Sort();
	TSet<FString> Seen;
	for (const FString& Folder : Folders)
	{
		const FString Key = FPaths::GetCleanFilename(Folder).ToLower();
		if (Seen.Contains(Key)) continue;
		Seen.Add(Key);
		if (auto Template = FModTemplateDescription::FromFolder(Folder))
		{
			ModTemplateDefinitions.Add(Template.ToSharedRef());
			TemplateDefinitions.Add(Template->TemplateDescription);
			if (Folder == Selected) CurrentTemplateDefinition = Template;
		}
	}
}

TSharedPtr<IPlugin> FModWizardDefinition::CreateModFromSelection(const FString& Name, const FString& Destination,
	const FPluginDescriptor& Descriptor, bool bShowContent, FText& Error)
{
	bPostCreationSucceeded = false;
	if (!HasValidTemplateSelection() || !FModTemplateDescription::FromFolder(GetSelectedTemplate()->OnDiskPath))
	{
		Error = LOCTEXT("InvalidSelection", "The selected template is no longer valid. Refresh and select a template.");
		return nullptr;
	}
	if (!IconOverridePath.IsEmpty() && !CrimeFactoryModIcon::IsValidPNG(IconOverridePath))
	{
		Error = LOCTEXT("InvalidCustomIcon", "The chosen icon is no longer readable. Choose a valid PNG icon.");
		return nullptr;
	}
	const FString SourceContent = GetSelectedTemplate()->OnDiskPath / TEXT("Content");
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *SourceContent, TEXT("*"), true, false);
	TArray<FCoreRedirect> Redirects;
	TMap<FSoftObjectPath, FSoftObjectPath> SoftPaths;
	TSet<FString> PackageNames;
	TArray<FString> AssetFiles;
	for (const FString& File : Files)
	{
		const FString Extension = FPaths::GetExtension(File);
		if (Extension != TEXT("uasset") && Extension != TEXT("umap")) continue;
		TUniquePtr<FArchive> Archive(IFileManager::Get().CreateFileReader(*File));
		if (!Archive) { Error = FText::FromString(TEXT("Cannot read template asset: ") + File); return nullptr; }
		FPackageFileSummary Summary;
		*Archive << Summary;
		if (Archive->IsError() || Summary.NameCount <= 0 || Summary.NameOffset < 0 ||
			Summary.NameOffset > Archive->TotalSize() || Summary.NameCount > Archive->TotalSize() / 4)
		{
			Error = FText::FromString(TEXT("Unsupported or invalid template package: ") + File);
			return nullptr;
		}
		// Older templates leave Summary.PackageName empty. Use Unreal's
		// versioned name-table serializer to find exact referenced package paths.
		Archive->SetUEVer(Summary.GetFileVersionUE());
		Archive->SetLicenseeUEVer(Summary.GetFileVersionLicenseeUE());
		Archive->SetCustomVersions(Summary.GetCustomVersionContainer());
		Archive->Seek(Summary.NameOffset);
		for (int32 Index = 0; Index < Summary.NameCount && !Archive->IsError(); ++Index)
		{
			FNameEntrySerialized Entry(ENAME_LinkerConstructor);
			*Archive << Entry;
			const FString Value = Entry.GetPlainNameString();
			if (FPackageName::IsValidLongPackageName(Value)) PackageNames.Add(Value);
		}
		if (Archive->IsError()) { Error = FText::FromString(TEXT("Cannot read template package names: ") + File); return nullptr; }
		AssetFiles.Add(File);
	}
	TSet<FString> OriginalPackages;
	for (FString File : AssetFiles)
	{
		FString Relative = File;
		FPaths::MakePathRelativeTo(Relative, *(SourceContent + TEXT("/")));
		Relative = FPaths::GetBaseFilename(Relative, false);
		FString OriginalPackage;
		for (const FString& Candidate : PackageNames)
		{
			const int32 RootEnd = Candidate.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
			if (RootEnd == INDEX_NONE || Candidate.Mid(RootEnd + 1) != Relative) continue;
			if (!OriginalPackage.IsEmpty() && OriginalPackage != Candidate)
			{
				Error = FText::FromString(TEXT("Ambiguous authoring package paths for template asset: ") + File);
				return nullptr;
			}
			OriginalPackage = Candidate;
		}
		if (OriginalPackage.IsEmpty()) OriginalPackage = TEXT("/") + FPaths::GetCleanFilename(GetSelectedTemplate()->OnDiskPath) + TEXT("/") + Relative;
		FString TargetPackage = TEXT("/") + Name + TEXT("/") + Relative;
		TargetPackage = TargetPackage.Replace(TEXT("PLUGIN_NAME"), *Name, ESearchCase::CaseSensitive);
		if (OriginalPackages.Contains(OriginalPackage))
		{
			Error = FText::FromString(TEXT("Duplicate original package in template: ") + OriginalPackage);
			return nullptr;
		}
		OriginalPackages.Add(OriginalPackage);
		if (OriginalPackage != TargetPackage)
			Redirects.Emplace(ECoreRedirectFlags::Type_Package, OriginalPackage, TargetPackage);
		const FString AssetName = FPackageName::GetShortName(OriginalPackage);
		const FString TargetName = FPackageName::GetShortName(TargetPackage);
		const FString OldObject = OriginalPackage + TEXT(".") + AssetName;
		const FString NewObject = TargetPackage + TEXT(".") + TargetName;
		SoftPaths.Add(FSoftObjectPath(OldObject), FSoftObjectPath(NewObject));
		SoftPaths.Add(FSoftObjectPath(OldObject + TEXT("_C")), FSoftObjectPath(NewObject + TEXT("_C")));
		SoftPaths.Add(FSoftObjectPath(OriginalPackage + TEXT(".Default__") + AssetName + TEXT("_C")),
			FSoftObjectPath(TargetPackage + TEXT(".Default__") + TargetName + TEXT("_C")));
	}
	const FString RedirectOwner = TEXT("CrimeFactory.Template.") + Name;
	if (!Redirects.IsEmpty() && !FCoreRedirects::AddRedirectList(Redirects, RedirectOwner))
	{
		FCoreRedirects::RemoveRedirectList(Redirects, RedirectOwner);
		Error = LOCTEXT("RedirectConflict", "Unable to register template package redirects. Check the Output Log.");
		return nullptr;
	}
	ON_SCOPE_EXIT { if (!Redirects.IsEmpty()) FCoreRedirects::RemoveRedirectList(Redirects, RedirectOwner); };
	FPluginUtils::FNewPluginParamsWithDescriptor Params;
	Params.TemplateFolders = GetFoldersForSelection();
	Params.Descriptor = Descriptor;
	GetPluginIconPath(Params.PluginIconPath);
	GetSelectedTemplate()->CustomizeDescriptorBeforeCreation(Params.Descriptor);
	FPluginUtils::FLoadPluginParams Load;
	Load.bEnablePluginInProject = true;
	Load.bUpdateProjectPluginSearchPath = true;
	Load.bSelectInContentBrowser = bShowContent;
	Load.OutFailReason = &Error;
	auto Plugin = FPluginUtils::CreateAndLoadNewPlugin(Name, Name, Destination, Params, Load);
	if (Plugin)
	{
		if (!IconOverridePath.IsEmpty() && !CrimeFactoryModIcon::CopyToResources(IconOverridePath, Plugin->GetBaseDir(), Error)) return Plugin;
		// PluginUtils removes its copy-time redirects before returning. Some
		// coincide with ours, so restore them for loading and post-processing.
		if (!Redirects.IsEmpty()) FCoreRedirects::AddRedirectList(Redirects, RedirectOwner);
		auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.ScanPathsSynchronous({Plugin->GetMountedAssetPath()}, true);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Plugin->GetMountedAssetPath()), Assets, true);
		TArray<UPackage*> Packages;
		TArray<UObject*> LoadedAssets;
		for (const FAssetData& Data : Assets)
		{
			UObject* Asset = Data.GetAsset();
			if (!Asset) { Error = FText::FromString(TEXT("Cannot load copied template asset: ") + Data.GetObjectPathString()); return Plugin; }
			LoadedAssets.Add(Asset);
			Packages.AddUnique(Asset->GetOutermost());
		}
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().RenameReferencingSoftObjectPaths(Packages, SoftPaths);
		if (!LoadedAssets.IsEmpty() && !UEditorAssetLibrary::SaveLoadedAssets(LoadedAssets, false))
		{
			Error = LOCTEXT("SaveTemplateReferences", "Unable to save copied template asset references. See the Output Log.");
			return Plugin;
		}
	}
	PluginCreated(Name, Plugin.IsValid());
	if (Plugin && bPostCreationSucceeded && UEditorAssetLibrary::DoesAssetExist(TEXT("/") + Name + TEXT("/DA_") + Name))
	{
		FString SyncError;
		if (!CrimeFactoryMetadataSync::Synchronize(*Plugin, true, SyncError))
		{
			bPostCreationSucceeded = false;
			Error = FText::FromString(SyncError);
		}
	}
	return Plugin;
}

const TArray<TSharedRef<FPluginTemplateDescription>>& FModWizardDefinition::GetTemplatesSource() const
{
	return TemplateDefinitions;
}

void FModWizardDefinition::OnTemplateSelectionChanged(TSharedPtr<FPluginTemplateDescription> InSelectedItem, ESelectInfo::Type SelectInfo)
{
	if (InSelectedItem.IsValid())
	{
		TSharedRef<FModTemplateDescription>* Found = ModTemplateDefinitions.FindByPredicate([InSelectedItem](TSharedRef<FModTemplateDescription> Template)
			{ return Template->TemplateDescription == InSelectedItem; });
		if(Found) {
			CurrentTemplateDefinition = *Found;
		}
	}
}

TSharedPtr<FPluginTemplateDescription> FModWizardDefinition::GetSelectedTemplate() const
{
	if(!CurrentTemplateDefinition.IsValid()) {
		return nullptr;
	}
	return CurrentTemplateDefinition->TemplateDescription;
}

bool FModWizardDefinition::HasValidTemplateSelection() const
{
	return CurrentTemplateDefinition.IsValid();
}

void FModWizardDefinition::ClearTemplateSelection()
{
	CurrentTemplateDefinition.Reset();
}

bool FModWizardDefinition::HasModules() const
{
	FString SourceFolderPath = GetPluginFolderPath() / TEXT("Source");
	
	return FPaths::DirectoryExists(SourceFolderPath);
}

bool FModWizardDefinition::IsMod() const
{
	return true;
}

FText FModWizardDefinition::GetInstructions() const
{
	return LOCTEXT("ChooseModTemplate", "Choose a template and then specify a name to create a new mod. Remember, your plugin name (mod reference) can no longer be changed after you release the mod! See the docs for more info.");
}

bool FModWizardDefinition::GetPluginIconPath(FString& OutIconPath) const
{
	if(!CurrentTemplateDefinition.IsValid()) {
		return false;
	}
	return GetTemplateIconPath(CurrentTemplateDefinition.ToSharedRef()->TemplateDescription, OutIconPath);
}

EHostType::Type FModWizardDefinition::GetPluginModuleDescriptor() const
{
	EHostType::Type ModuleDescriptorType = EHostType::Runtime;

	if (CurrentTemplateDefinition.IsValid())
	{
		ModuleDescriptorType = CurrentTemplateDefinition->TemplateDescription->ModuleDescriptorType;
	}

	return ModuleDescriptorType;
}

ELoadingPhase::Type FModWizardDefinition::GetPluginLoadingPhase() const
{
	ELoadingPhase::Type Phase = ELoadingPhase::Default;

	if (CurrentTemplateDefinition.IsValid())
	{
		Phase = CurrentTemplateDefinition->TemplateDescription->LoadingPhase;
	}

	return Phase;
}

bool FModWizardDefinition::GetTemplateIconPath(TSharedRef<FPluginTemplateDescription> Template, FString& OutIconPath) const
{


	FString TemplateFolderName = GetFolderForTemplate(Template);

	OutIconPath = TemplateFolderName / TEXT("Resources/Icon128.png");
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*OutIconPath))
	{
		OutIconPath = PluginBaseDir / TEXT("Resources/CrimeFactory128.png");

	}

	return FPaths::FileExists(OutIconPath);
}

TArray<FString> FModWizardDefinition::GetFoldersForSelection() const
{
	TArray<FString> SelectedFolders;

	if (CurrentTemplateDefinition.IsValid())
	{
		SelectedFolders.Add(GetFolderForTemplate(CurrentTemplateDefinition.ToSharedRef()->TemplateDescription));
	}

	return SelectedFolders;
}

void FModWizardDefinition::RenameTemplateAssetsForMods(const FString& PluginName) const
{
	bPostCreationSucceeded = false;
	UE_LOG(
		LogCrimeFactory,
		Display,
		TEXT("[TemplatePALFixV2] RenameTemplateAssetsForMods running for '%s'"),
		*PluginName
	);

	// Find the mounted asset path of the target plugin.
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Plugin not found: %s"), *PluginName);
		return;
	}

	const FString PluginContentPath = Plugin->GetMountedAssetPath();

	/*
	 * A mod plugin has just been copied/created, so do not assume the Asset
	 * Registry has already discovered its template assets. Scan the mounted
	 * plugin root synchronously before trying to find PLACEHOLDER_* assets.
	 */
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FString> PathsToScan;
	PathsToScan.Add(PluginContentPath);

	AssetRegistry.ScanPathsSynchronous(
		PathsToScan,
		true // bForceRescan
	);
	// AssetTools rejects renames while any asset discovery is still running,
	// even after this plugin's own directory has been scanned synchronously.
	AssetRegistry.WaitForCompletion();
	if (AssetRegistry.IsLoadingAssets())
	{
		UE_LOG(LogCrimeFactory, Error, TEXT("Cannot rename template assets for %s while asset discovery is incomplete."), *PluginName);
		return;
	}
	ON_SCOPE_EXIT
	{
		CleanupTemplateRedirectors(PluginContentPath);
		// The content-mounted callback deliberately skips template PAL names.
		// Persist the final PAL path only after renaming and redirector cleanup.
		FModuleManager::GetModuleChecked<FCrimeFactoryModule>(TEXT("CrimeFactory"))
			.EnsureAssetManagerRegistrationForPlugin(*Plugin);
	};

	TArray<FAssetData> AssetDataArray;
	AssetRegistry.GetAssetsByPath(
		FName(*PluginContentPath),
		AssetDataArray,
		true // bRecursive
	);

	UE_LOG(
		LogTemp,
		Log,
		TEXT("Found %d template assets under '%s'."),
		AssetDataArray.Num(),
		*PluginContentPath
	);

	TArray<FAssetRenameData> RenameDataList;

	const auto ResolvePlaceholderName =
		[&PluginName](const FString& InName, FString& OutName) -> bool
		{
			if (InName.EndsWith(TEXT("_PLACEHOLDER_MODNAME")))
			{
				OutName = InName.LeftChop(19) + PluginName;
				return true;
			}
			if (!InName.StartsWith(TEXT("PLACEHOLDER_")) ||
				!InName.EndsWith(TEXT("MODNAME")))
			{
				OutName = InName;
				return false;
			}

			// Same naming rule used for template assets:
			//   PLACEHOLDER_DA_HEIST_MODNAME -> DA_HEIST_<PluginName>
			//   PLACEHOLDER_MODNAME          -> <PluginName>
			OutName = InName.RightChop(12);
			OutName = OutName.LeftChop(7);
			OutName += PluginName;
			return true;
		};

	const auto ResolvePlaceholderPackagePath =
		[&ResolvePlaceholderName](
			const FString& InPackagePath,
			FString& OutPackagePath) -> bool
		{
			TArray<FString> Segments;
			InPackagePath.ParseIntoArray(
				Segments,
				TEXT("/"),
				true
			);

			bool bChanged = false;

			for (FString& Segment : Segments)
			{
				FString ResolvedSegment;
				if (ResolvePlaceholderName(
						Segment,
						ResolvedSegment))
				{
					Segment = MoveTemp(ResolvedSegment);
					bChanged = true;
				}
			}

			OutPackagePath =
				(InPackagePath.StartsWith(TEXT("/"))
					? TEXT("/")
					: TEXT("")) +
				FString::Join(Segments, TEXT("/"));

			return bChanged;
		};

	int32 AssetsWithRenamedNames = 0;
	int32 AssetsMovedFromPlaceholderFolders = 0;

	for (const FAssetData& AssetData : AssetDataArray)
	{
		// A retry can encounter redirectors left by an earlier rename. They
		// are cleanup candidates, not assets to rename onto their destinations.
		if (AssetData.IsRedirector()) continue;
		const FString OldName = AssetData.AssetName.ToString();
		const FString OldPackagePath = AssetData.PackagePath.ToString();

		FString NewName;
		const bool bRenameAsset =
			ResolvePlaceholderName(OldName, NewName);

		FString NewPackagePath;
		const bool bMoveAsset =
			ResolvePlaceholderPackagePath(
				OldPackagePath,
				NewPackagePath
			);

		if (!bRenameAsset && !bMoveAsset)
		{
			continue;
		}

		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			UE_LOG(
				LogTemp,
				Warning,
				TEXT("Could not load template asset '%s' for rename/move."),
				*AssetData.GetObjectPathString()
			);
			return;
		}

		RenameDataList.Add(
			FAssetRenameData(
				Asset,
				bMoveAsset
					? NewPackagePath
					: OldPackagePath,
				bRenameAsset
					? NewName
					: OldName
			)
		);

		AssetsWithRenamedNames += bRenameAsset ? 1 : 0;
		AssetsMovedFromPlaceholderFolders += bMoveAsset ? 1 : 0;
	}

	if (RenameDataList.Num() > 0)
	{
		// Use AssetTools to rename assets and update references safely.
		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");

		const bool bRenameSucceeded =
			AssetToolsModule.Get().RenameAssets(RenameDataList);

		if (bRenameSucceeded)
		{
			UE_LOG(
				LogTemp,
				Log,
				TEXT(
					"Renamed/moved %d template asset(s) in plugin '%s' "
					"(%d asset-name rename(s), %d asset move(s) from placeholder folder(s))."
				),
				RenameDataList.Num(),
				*PluginName,
				AssetsWithRenamedNames,
				AssetsMovedFromPlaceholderFolders
			);
		}
		else
		{
			UE_LOG(
				LogTemp,
				Error,
				TEXT("One or more template assets failed to rename in plugin '%s'."),
				*PluginName
			);
			return;
		}
	}
	else
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT(
				"No assets or package folders matched PLACEHOLDER_*MODNAME "
				"rename criteria in plugin '%s'."
			),
			*PluginName
		);
	}

	/*
	 * Fix the PAL's explicit DA reference.
	 *
	 * The previous code unconditionally wrote ExplicitAssets[0]. A freshly
	 * created PAL is allowed to have zero explicit assets, which caused:
	 *
	 *   Array index out of bounds: 0 into an array of size 0
	 *
	 * Preserve any additional explicit assets, but create element 0 when the
	 * array is empty.
	 */
	const FString PALName =
		PluginContentPath +
		FString::Printf(TEXT("PAL_%s"), *PluginName);

	if (!UEditorAssetLibrary::DoesAssetExist(PALName)) { bPostCreationSucceeded = true; return; }
	UPrimaryAssetLabel* Label =
		Cast<UPrimaryAssetLabel>(
			UEditorAssetLibrary::LoadAsset(*PALName)
		);

	if (!Label)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("Failed to load PrimaryAssetLabel at %s"),
			*PALName
		);
		return;
	}

	const FString DataAssetName =
		PluginContentPath +
		FString::Printf(TEXT("DA_%s"), *PluginName) +
		TEXT(".") +
		FString::Printf(TEXT("DA_%s"), *PluginName);

	const FSoftObjectPath DataAssetPath(*DataAssetName);

	// The metadata reference is optional for community templates.
	if (!UEditorAssetLibrary::DoesAssetExist(DataAssetName)) { bPostCreationSucceeded = true; return; }
	// Keep community PAL entries. Replace only the conventional metadata
	// placeholder, then ensure the generated metadata is included.
	Label->ExplicitAssets.RemoveAll([](const TSoftObjectPtr<UObject>& Entry)
	{
		return Entry.ToSoftObjectPath().GetAssetName() == TEXT("PLACEHOLDER_DA_MODNAME");
	});
	
	const TSoftObjectPtr DataAssetSoftPtr(DataAssetPath);
	Label->ExplicitAssets.AddUnique(DataAssetSoftPtr);

	// Load directly from the soft path rather than indexing ExplicitAssets.
	UObject* LoadedAsset = DataAssetPath.TryLoad();

	if (!LoadedAsset)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("Failed to load new explicit asset %s"),
			*DataAssetName
		);
	}
	else
	{
		UE_LOG(
			LogTemp,
			Log,
			TEXT("Loaded new explicit asset %s successfully"),
			*DataAssetName
		);
	}

	// Mark package dirty for Editor.
	Label->MarkPackageDirty();

	// Save the updated asset.
	UEditorAssetSubsystem* AssetSubsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
			: nullptr;

	if (AssetSubsystem && AssetSubsystem->SaveAsset(*PALName))
	{
		bPostCreationSucceeded = true;
		UE_LOG(
			LogTemp,
			Log,
			TEXT("Successfully replaced explicit asset and saved %s"),
			*PALName
		);
	}
	else
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("Failed to save PrimaryAssetLabel asset %s"),
			*PALName
		);
	}
}


bool FModWizardDefinition::PopulateGeneratedModDataAssetForPlugin(const FString& PluginName) const
{
	return PopulateGeneratedModDataAsset(PluginName);
}

void FModWizardDefinition::PluginCreated(const FString& PluginName, bool bWasSuccessful) const
{
	bPostCreationSucceeded = false;
	if(!bWasSuccessful)
	{
		UE_LOG(LogCrimeFactory, Error, TEXT("Failed to create mod %s"), *PluginName);
		return;
	}

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);

	if(!Plugin.IsValid())
	{
		UE_LOG(LogCrimeFactory, Error, TEXT("Created mod %s was invalid"), *PluginName);
		return;
	}

	/*
	 * Write the dependency directly into the .uplugin JSON.
	 *
	 * In this UE5.5 CrimeFactory path, FPluginDescriptor::Plugins was not being
	 * persisted even though other descriptor fields (for example SemVersion)
	 * were. Writing the JSON directly makes the dependency deterministic.
	 *
	 * Do this BEFORE RenameTemplateAssetsForMods(), because those template
	 * assets can reference /PDML and AssetReferenceRestrictions needs to see
	 * the dependency first.
	 */
	const FString NewPluginDescriptorFile =
		Plugin->GetDescriptorFileName();

	if (!EnsurePDMLDependencyInUPlugin(
			NewPluginDescriptorFile,
			PluginName))
	{
		UE_LOG(
			LogCrimeFactory,
			Error,
			TEXT("Unable to add required PDML dependency to mod %s"),
			*PluginName
		);
		return;
	}

	// Reload plugin descriptors so AssetReferenceRestrictions sees PDML as a
	// dependency of the newly-created mod in this same editor session.
	IPluginManager::Get().RefreshPluginsList();
	Plugin = IPluginManager::Get().FindPlugin(PluginName);

	if (!Plugin.IsValid())
	{
		UE_LOG(
			LogCrimeFactory,
			Error,
			TEXT("Mod %s was not found after refreshing the plugin list"),
			*PluginName
		);
		return;
	}

	// Configuration is text, unlike asset packages. Only complete, explicitly
	// delimited naming tokens are substituted; assets go through AssetTools below.
	TArray<FString> ConfigFiles;
	IFileManager::Get().FindFilesRecursive(ConfigFiles, *(Plugin->GetBaseDir() / TEXT("Config")), TEXT("*.ini"), true, false);
	const FRegexPattern TokenPattern(TEXT("\\bPLACEHOLDER_(?:[A-Za-z0-9]+_)*MODNAME\\b"));
	for (const FString& ConfigFile : ConfigFiles)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *ConfigFile))
		{
			UE_LOG(LogCrimeFactory, Error, TEXT("Cannot read generated configuration %s"), *ConfigFile);
			return;
		}
		FRegexMatcher Matcher(TokenPattern, Text);
		FString Updated;
		int32 Last = 0;
		while (Matcher.FindNext())
		{
			const FString Token = Matcher.GetCaptureGroup(0);
			Updated += Text.Mid(Last, Matcher.GetMatchBeginning() - Last);
			Updated += Token.Mid(12, Token.Len() - 19) + PluginName;
			Last = Matcher.GetMatchEnding();
		}
		if (Last > 0)
		{
			Updated += Text.Mid(Last);
			if (!FFileHelper::SaveStringToFile(Updated, *ConfigFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogCrimeFactory, Error, TEXT("Cannot save generated configuration %s"), *ConfigFile);
				return;
			}
		}
	}

	// Initialize required fields
	FPluginDescriptor Descriptor = Plugin->GetDescriptor();
	if (Descriptor.FriendlyName.IsEmpty()) {
		Descriptor.FriendlyName = PluginName;
	}
	// Core fields
	Descriptor.Version = 1;
	Descriptor.VersionName = TEXT("1.0.0");
	Descriptor.bCanContainContent = true;
	Descriptor.EnabledByDefault = EPluginEnabledByDefault::Enabled;
	Descriptor.AdditionalFieldsToWrite.Add( TEXT("SemVersion"), MakeShared<FJsonValueString>( TEXT("1.0.0") ) );
	
	// Initialize extra dependencies
	for (const TSharedPtr<FModTemplateDependency>& Dependency : CurrentTemplateDefinition->Dependencies) {
		if (Descriptor.Plugins.ContainsByPredicate([&Dependency](const FPluginReferenceDescriptor& Reference) { return Reference.Name.Equals(Dependency->Name, ESearchCase::IgnoreCase); })) continue;
		FPluginReferenceDescriptor DependencyDescriptor(Dependency->Name, true);
		DependencyDescriptor.AdditionalFieldsToWrite.Add( TEXT("SemVersion"), MakeShared<FJsonValueString>( TEXT("^") + Dependency->Version ) );
		if(Dependency->bOptional) {
			DependencyDescriptor.bOptional = true;
		}
		if(Dependency->bBasePlugin) {
			DependencyDescriptor.AdditionalFieldsToWrite.Add( TEXT("BasePlugin"), MakeShared<FJsonValueBoolean>( true ) );
		}
		Descriptor.Plugins.Add(DependencyDescriptor);
	}


	FText FailReason;
	if(!Plugin->UpdateDescriptor(Descriptor, FailReason))
	{
		UE_LOG(LogCrimeFactory, Error, TEXT("Failed to update descriptor for mod %s: %s"), *PluginName, *FailReason.ToString());
		return;
	}

	// Explicitly update the .uplugin on disk as well. The dependency is already
	// injected before creation, but this makes the post-create path deterministic
	// and preserves any fields written by the Plugin Browser.
	FText PluginFileFailReason;
	if (!Descriptor.UpdatePluginFile(Plugin->GetDescriptorFileName(), PluginFileFailReason))
	{
		UE_LOG(
			LogCrimeFactory,
			Error,
			TEXT("Failed to write descriptor file for mod %s: %s"),
			*PluginName,
			*PluginFileFailReason.ToString()
		);
		return;
	}

	if (!EnsurePDMLDependencyInUPlugin(
			Plugin->GetDescriptorFileName(),
			PluginName))
	{
		return;
	}

	IPluginManager::Get().RefreshPluginsList();

	UE_LOG(
		LogCrimeFactory,
		Display,
		TEXT("Verified PDML dependency in descriptor for mod %s: %s"),
		*PluginName,
		*Plugin->GetDescriptorFileName()
	);

	UE_LOG(LogCrimeFactory, Log, TEXT("Created mod %s"), *PluginName);

	RenameTemplateAssetsForMods(PluginName);

	/*
	 * The template assets/folders now have their final names, so populate
	 * the generated PDA_ModMeta instance, discover ModActor classes and
	 * custom-heist Data Assets, and repair heist LevelPath references.
	 */
	if (bPostCreationSucceeded && UEditorAssetLibrary::DoesAssetExist(TEXT("/") + PluginName + TEXT("/DA_") + PluginName))
		bPostCreationSucceeded = PopulateGeneratedModDataAsset(PluginName);
}

FString FModWizardDefinition::GetPluginFolderPath() const
{
	return FPaths::Combine(FPaths::ProjectDir(), TEXT("Mods"));
}

FString FModWizardDefinition::GetFolderForTemplate(TSharedRef<FPluginTemplateDescription> InTemplate) const
{
	return InTemplate->OnDiskPath;
}

TSharedPtr<FModTemplateDescription> FModTemplateDescription::FromFolder(const FString& Folder)
{
	const FString Name = FPaths::GetCleanFilename(Folder);
	// Reject linked trees before recursive copy or inventory scans. A community
	// template must be self-contained and cannot redirect writes or traversal.
	IPlatformFile& FileSystem = FPlatformFileManager::Get().GetPlatformFile();
	if (FileSystem.IsSymlink(*Folder) == ESymlinkResult::Symlink) return nullptr;
	bool bSupportedTree = true;
	TArray<FString> Pending{Folder};
	while (!Pending.IsEmpty() && bSupportedTree)
	{
		const FString Directory = Pending.Pop();
		if (!FileSystem.IterateDirectory(*Directory, [&](const TCHAR* Path, bool bDirectory)
		{
			const FString Leaf = FPaths::GetCleanFilename(Path);
			if (FileSystem.IsSymlink(Path) == ESymlinkResult::Symlink ||
				Leaf == TEXT("__ExternalActors__") || Leaf == TEXT("__ExternalObjects__"))
			{
				bSupportedTree = false;
				return false;
			}
			if (bDirectory) Pending.Add(Path);
			return true;
		})) bSupportedTree = false;
	}
	if (!bSupportedTree)
	{
		UE_LOG(LogCrimeFactory, Warning, TEXT("Ignoring unreadable or unsupported template tree (linked or external actor content): %s"), *Folder);
		return nullptr;
	}
	// Content-only templates are supported. Native source and descriptors cannot
	// safely be treated as content templates by this wizard.
	if (Name.StartsWith(TEXT(".")) || !FPaths::ValidatePath(Name) ||
		IFileManager::Get().DirectoryExists(*(Folder / TEXT("Source")))) return nullptr;
	TArray<FString> Descriptors;
	IFileManager::Get().FindFiles(Descriptors, *(Folder / TEXT("*.uplugin")), true, false);
	if (!Descriptors.IsEmpty()) return nullptr;
	bool bHasStructure = false;
	for (const TCHAR* Part : {TEXT("Content"), TEXT("Config"), TEXT("Resources")})
		bHasStructure |= IFileManager::Get().DirectoryExists(*(Folder / Part));
	if (!bHasStructure)
	{
		UE_LOG(LogCrimeFactory, Warning, TEXT("Ignoring template without Content, Config or Resources: %s"), *Folder);
		return nullptr;
	}
	FString Description;
	FFileHelper::LoadFileToString(Description, *(Folder / TEXT("Description.txt")));
	Description = Description.TrimStartAndEnd();
	if (Description.IsEmpty()) Description = TEXT("Create a mod from the ") + Name + TEXT(" template.");
	TArray<TSharedPtr<FModTemplateDependency>> Dependencies;
	TArray<FString> Names;
	FFileHelper::LoadFileToStringArray(Names, *(Folder / TEXT("Dependencies.txt")));
	for (FString DependencyName : Names)
	{
		DependencyName.TrimStartAndEndInline();
		if (DependencyName.IsEmpty() || DependencyName.StartsWith(TEXT("#"))) continue;
		const auto Plugin = IPluginManager::Get().FindPlugin(DependencyName);
		Dependencies.Add(MakeShared<FModTemplateDependency>(DependencyName,
			Plugin ? Plugin->GetDescriptor().VersionName : FString(), false, false));
	}
	auto Template = MakeShared<FCrimeFactoryPluginTemplateDescription>(
		FText::FromString(FName::NameToDisplayString(Name, false)), FText::FromString(Description), Folder, true, EHostType::Runtime);
	for (const auto& Dependency : Dependencies) Template->RequiredPluginNames.AddUnique(Dependency->Name);
	Template->bCanBePlacedInEngine = false;
	return MakeShared<FModTemplateDescription>(Template, Dependencies);
}

#undef LOCTEXT_NAMESPACE
