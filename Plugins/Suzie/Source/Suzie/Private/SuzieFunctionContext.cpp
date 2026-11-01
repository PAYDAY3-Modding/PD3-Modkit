#include "SuzieFunctionContext.h"
#include "SuziePlugin.h"
#include "SuzieNativeLifecycle.h"

namespace
{
    void CollectNativeStructReferences(FProperty& P, void* Address, FReferenceCollector& Collector)
    {
        for (int32 Index = 0; Index < P.ArrayDim; ++Index)
        {
            void* Value = static_cast<uint8*>(Address) + Index * P.GetElementSize();
            if (const auto* Struct = CastField<FStructProperty>(&P))
            { Collector.AddPropertyReferencesWithStructARO(Struct->Struct, Value); }
            else if (const auto* Array = CastField<FArrayProperty>(&P))
            {
                FScriptArrayHelper Items(Array, Value);
                for (int32 I = 0; I < Items.Num(); ++I) { CollectNativeStructReferences(*Array->Inner, Items.GetRawPtr(I), Collector); }
            }
            else if (const auto* Set = CastField<FSetProperty>(&P))
            {
                FScriptSetHelper Items(Set, Value);
                for (int32 I = 0; I < Items.GetMaxIndex(); ++I)
                { if (Items.IsValidIndex(I)) { CollectNativeStructReferences(*Set->ElementProp, Items.GetElementPtr(I), Collector); } }
            }
            else if (const auto* Map = CastField<FMapProperty>(&P))
            {
                FScriptMapHelper Items(Map, Value);
                for (int32 I = 0; I < Items.GetMaxIndex(); ++I)
                {
                    if (Items.IsValidIndex(I))
                    {
                        CollectNativeStructReferences(*Map->KeyProp, Items.GetKeyPtr(I), Collector);
                        CollectNativeStructReferences(*Map->ValueProp, Items.GetValuePtr(I), Collector);
                    }
                }
            }
        }
    }
}

FSuzieFunctionContext::FSuzieFunctionContext(UObject* InSelf, UFunction& InFunction, bool bInSetup)
    : Self(InFunction.HasAnyFunctionFlags(FUNC_Static) ? InFunction.GetOuterUClass()->GetDefaultObject() : InSelf),
      Function(&InFunction), Parameters(&InFunction), bSetup(bInSetup)
{
}

FSuzieFunctionContext::FSuzieFunctionContext(UObject* InSelf, ESuzieCallbackPhase InPhase, FName InChangedProperty, FName InChangedLeaf, const UClass* InPropertyScope)
    : Self(InSelf), Function(nullptr), PropertyScope(InPropertyScope), bSetup(false), Phase(InPhase), ChangedProperty(InChangedProperty), ChangedLeaf(InChangedLeaf)
{
}

void FSuzieFunctionContext::Fail(const FString& Message)
{
    if (Error.IsEmpty())
    {
        Error = Message;
        UE_LOG(LogSuzie, Error, TEXT("Native body %s: %s"), *GetPathNameSafe(Function ? static_cast<UObject*>(Function.Get()) : Self.Get()), *Message);
    }
}

void FSuzieFunctionContext::AddReferencedObjects(FReferenceCollector& Collector)
{
    Collector.AddReferencedObject(Self);
    Collector.AddReferencedObject(Function);
    Collector.AddReferencedObject(PropertyScope);
    // FStructOnScope::AddReferencedObjects casts its type to UScriptStruct. This frame is
    // a UFunction, so collect its properties explicitly, including native nested-struct AROs.
    if (Function)
    {
        Collector.AddPropertyReferences(Function, GetParameterMemory());
        for (TFieldIterator<FProperty> It(Function.Get()); It; ++It)
        { CollectNativeStructReferences(**It, It->ContainerPtrToValuePtr<void>(GetParameterMemory()), Collector); }
    }
}

FSuzieReflectionValue FSuzieFunctionContext::Param(FName Name)
{
    FProperty* P = Function ? FindFProperty<FProperty>(Function, Name) : nullptr;
    if (!P || !P->HasAnyPropertyFlags(CPF_Parm) || P->HasAnyPropertyFlags(CPF_ReturnParm))
    {
        Fail(FString::Printf(TEXT("Parameter '%s' does not exist"), *Name.ToString()));
        return FSuzieReflectionValue(*this, nullptr, nullptr, false);
    }
    const bool bWritable = bSetup || !P->HasAnyPropertyFlags(CPF_ConstParm);
    P = ResolveParameterAlias(P);
    return FSuzieReflectionValue(*this, P, P->ContainerPtrToValuePtr<void>(GetParameterMemory()), bWritable);
}

FSuzieReflectionValue FSuzieFunctionContext::Property(FName Name, UObject* Owner)
{
    if (!Owner) { Owner = Self; }
    const UStruct* Scope = ::IsValid(Owner) ? (Owner == Self && PropertyScope ? PropertyScope.Get() : Owner->GetClass()) : nullptr;
    FProperty* P = Scope ? FindFProperty<FProperty>(Scope, Name) : nullptr;
    if (!P)
    {
        Fail(FString::Printf(TEXT("Object property '%s' does not exist"), *Name.ToString()));
        return FSuzieReflectionValue(*this, nullptr, nullptr, false);
    }
    const bool bWritable = Owner != Self || !Function || !Function->HasAnyFunctionFlags(FUNC_Const);
    return FSuzieReflectionValue(*this, P, P->ContainerPtrToValuePtr<void>(Owner), bWritable);
}

FSuzieReflectionValue FSuzieFunctionContext::Return()
{
    FProperty* P = Function ? Function->GetReturnProperty() : nullptr;
    if (!P) { Fail(TEXT("Function has no return value")); }
    return FSuzieReflectionValue(*this, P, P ? P->ContainerPtrToValuePtr<void>(GetParameterMemory()) : nullptr, true);
}

TUniquePtr<FSuzieFunctionContext> FSuzieFunctionContext::CallFunction(FName Name,
    TFunction<void(FSuzieFunctionContext&)> Setup, UObject* Target)
{
    if (!IsInGameThread()) { Fail(TEXT("Reflected calls require the game thread; use SUZIE_POST_INITIALIZE")); return nullptr; }
    if (!Target) { Target = Self; }
    UFunction* Callee = ::IsValid(Target) ? Target->FindFunction(Name) : nullptr;
    if (!Callee || Callee->HasAnyFunctionFlags(FUNC_Net | FUNC_Delegate | FUNC_MulticastDelegate) || Callee->HasMetaData(TEXT("Latent")))
    {
        Fail(FString::Printf(TEXT("Cannot call missing, asynchronous, RPC or delegate function '%s'"), *Name.ToString()));
        return nullptr;
    }
    if (Callee->HasAnyFunctionFlags(FUNC_Static)) { Target = Callee->GetOuterUClass()->GetDefaultObject(); }
    auto Call = MakeUnique<FSuzieFunctionContext>(Target, *Callee, true);
    if (Setup) { Setup(*Call); }
    Call->bSetup = false;
    if (!Call->IsValid()) { Fail(Call->GetError()); return nullptr; }
    Target->ProcessEvent(Callee, Call->GetParameterMemory());
    return Call;
}

void FSuzieFunctionContext::WatchDependency(UObject* Dependency, FName Property)
{ if (Self && IsInGameThread()) { SuzieNativeLifecycle::WatchDependency(*Self, Dependency, Property); } }

void FSuzieFunctionContext::ClearDependencies()
{ if (Self && IsInGameThread()) { SuzieNativeLifecycle::ClearDependencies(*Self); } }

void FSuzieFunctionContext::RequestRefresh()
{ if (Self && IsInGameThread()) { SuzieNativeLifecycle::RequestRefresh(*Self); } }

void FSuzieFunctionContext::ObserveWidget()
{
    if (Self && IsInGameThread() && !SuzieNativeLifecycle::ObserveWidget(*Self))
    { Fail(TEXT("ObserveWidget requires a UWidget with a SUZIE_REFRESH callback")); }
}

FSuzieReflectionValue::FSuzieReflectionValue(FSuzieFunctionContext& InContext, FProperty* InProperty, void* InAddress, bool bInWritable)
    : Context(&InContext), Property(InProperty), Address(InAddress), bWritable(bInWritable)
{
}

void FSuzieReflectionValue::Fail(const FString& Message) const
{
    Context->Fail(FString::Printf(TEXT("%s (%s)"), *Message, Property ? *Property->GetPathName() : TEXT("missing property")));
}

bool FSuzieReflectionValue::CheckType(bool bMatches) const
{
    if (!IsValid() || !bMatches) { Fail(TEXT("Requested C++ type does not match the reflected property")); return false; }
    return true;
}

bool FSuzieReflectionValue::CanWrite() const
{
    if (!IsValid() || !bWritable) { Fail(TEXT("Cannot write a missing or const value")); return false; }
    return Context->IsValid();
}

FSuzieReflectionValue FSuzieReflectionValue::Field(FName Name) const
{
    const auto* Struct = CastField<FStructProperty>(Property);
    FProperty* P = Struct && Address ? FindFProperty<FProperty>(Struct->Struct, Name) : nullptr;
    if (!P) { Fail(FString::Printf(TEXT("Struct field '%s' does not exist"), *Name.ToString())); }
    return FSuzieReflectionValue(*Context, P, P ? P->ContainerPtrToValuePtr<void>(Address) : nullptr, bWritable);
}

FSuzieReflectionValue FSuzieReflectionValue::Element(int32 Index) const
{
    const auto* Array = CastField<FArrayProperty>(Property);
    if (Array && Address)
    {
        FScriptArrayHelper Values(Array, Address);
        if (Values.IsValidIndex(Index)) { return FSuzieReflectionValue(*Context, Array->Inner, Values.GetRawPtr(Index), bWritable); }
    }
    Fail(FString::Printf(TEXT("Array index %d is invalid"), Index));
    return FSuzieReflectionValue(*Context, nullptr, nullptr, false);
}

int32 FSuzieReflectionValue::Num() const
{
    if (Address)
    {
        if (const auto* P = CastField<FArrayProperty>(Property)) { return FScriptArrayHelper(P, Address).Num(); }
        if (const auto* P = CastField<FSetProperty>(Property)) { return FScriptSetHelper(P, Address).Num(); }
        if (const auto* P = CastField<FMapProperty>(Property)) { return FScriptMapHelper(P, Address).Num(); }
    }
    Fail(TEXT("Value is not an array, set or map"));
    return 0;
}

namespace
{
    const FNumericProperty* EnumStorage(const FProperty* P, UEnum*& Enum)
    {
        if (const auto* E = CastField<FEnumProperty>(P)) { Enum = E->GetEnum(); return E->GetUnderlyingProperty(); }
        if (const auto* E = CastField<FByteProperty>(P); E && E->Enum) { Enum = E->Enum; return E; }
        Enum = nullptr;
        return nullptr;
    }
}

int64 FSuzieReflectionValue::GetEnum() const
{
    UEnum* Enum;
    const auto* Storage = EnumStorage(Property, Enum);
    if (!CheckType(Storage && Enum)) { return 0; }
    return Storage->GetSignedIntPropertyValue(Address);
}

bool FSuzieReflectionValue::SetEnum(int64 Value) const
{
    UEnum* Enum;
    const auto* Storage = EnumStorage(Property, Enum);
    if (!CanWrite() || !CheckType(Storage && Enum)) { return false; }
    if (!Enum->IsValidEnumValueOrBitfield(Value)) { Fail(TEXT("Unknown enum value")); return false; }
    Storage->SetIntPropertyValue(Address, Value);
    return true;
}

bool FSuzieReflectionValue::SetEnum(FName Enumerator) const
{
    UEnum* Enum;
    EnumStorage(Property, Enum);
    if (!Enum) { Fail(TEXT("Value is not an enum")); return false; }
    const int64 Value = Enum->GetValueByNameString(Enumerator.ToString());
    if (Value == INDEX_NONE) { Fail(TEXT("Unknown enum name")); return false; }
    return SetEnum(Value);
}
