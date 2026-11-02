#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "UObject/UnrealType.h"
#include <type_traits>

// Every conversion checks the reflected representation before accessing its storage.
// Containers are converted element by element; their C++ layouts are never assumed.
namespace SuzieReflection
{
    template<class T> struct TValue
    {
        static bool Matches(const FProperty* Property)
        {
            if (!Property || Property->ArrayDim != 1) { return false; }
            if constexpr ((std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) ||
                std::is_same_v<T, FString> || std::is_same_v<T, FName> || std::is_same_v<T, FText>)
            { if (Property->GetSize() != sizeof(T)) { return false; } }
            if constexpr (std::is_same_v<T, bool>) { return Property->IsA<FBoolProperty>(); }
            else if constexpr (std::is_same_v<T, int8>) { return Property->IsA<FInt8Property>(); }
            else if constexpr (std::is_same_v<T, uint8>) { const auto* P = CastField<FByteProperty>(Property); return P && !P->Enum; }
            else if constexpr (std::is_same_v<T, int16>) { return Property->IsA<FInt16Property>(); }
            else if constexpr (std::is_same_v<T, uint16>) { return Property->IsA<FUInt16Property>(); }
            else if constexpr (std::is_same_v<T, int32>) { return Property->IsA<FIntProperty>(); }
            else if constexpr (std::is_same_v<T, uint32>) { return Property->IsA<FUInt32Property>(); }
            else if constexpr (std::is_same_v<T, int64>) { return Property->IsA<FInt64Property>(); }
            else if constexpr (std::is_same_v<T, uint64>) { return Property->IsA<FUInt64Property>(); }
            else if constexpr (std::is_same_v<T, float>) { return Property->IsA<FFloatProperty>(); }
            else if constexpr (std::is_same_v<T, double>) { return Property->IsA<FDoubleProperty>(); }
            else if constexpr (std::is_same_v<T, FString>) { return Property->IsA<FStrProperty>(); }
            else if constexpr (std::is_same_v<T, FName>) { return Property->IsA<FNameProperty>(); }
            else if constexpr (std::is_same_v<T, FText>) { return Property->IsA<FTextProperty>(); }
            else if constexpr (std::is_pointer_v<T> && std::is_convertible_v<T, UObject*>)
            {
                const auto* P = CastField<FObjectPropertyBase>(Property);
                return P && P->PropertyClass->IsChildOf(std::remove_pointer_t<T>::StaticClass());
            }
            else if constexpr (std::is_enum_v<T>)
            {
                if (const auto* P = CastField<FEnumProperty>(Property)) { return P->GetEnum() == StaticEnum<T>(); }
                if (const auto* P = CastField<FByteProperty>(Property)) { return P->Enum == StaticEnum<T>(); }
                return false;
            }
            else
            {
                const auto* P = CastField<FStructProperty>(Property);
                const UScriptStruct* Expected;
                if constexpr (requires { T::StaticStruct(); }) { Expected = T::StaticStruct(); }
                else { Expected = TBaseStructure<T>::Get(); }
                return P && P->Struct == Expected && P->Struct->GetStructureSize() == sizeof(T);
            }
        }

        static bool Read(const FProperty* P, const void* Address, T& Out)
        {
            if constexpr (std::is_same_v<T, bool>) { Out = CastFieldChecked<FBoolProperty>(P)->GetPropertyValue(Address); }
            else if constexpr (std::is_pointer_v<T> && std::is_convertible_v<T, UObject*>)
            { Out = Cast<std::remove_pointer_t<T>>(CastFieldChecked<FObjectPropertyBase>(P)->GetObjectPropertyValue(Address)); }
            else if constexpr (std::is_enum_v<T>)
            {
                const auto* Enum = CastField<FEnumProperty>(P);
                const FNumericProperty* Number = Enum ? Enum->GetUnderlyingProperty() : CastFieldChecked<FByteProperty>(P);
                Out = static_cast<T>(Number->GetSignedIntPropertyValue(Address));
            }
            else { P->CopyCompleteValue(&Out, Address); }
            return true;
        }

        static bool Write(const FProperty* P, void* Address, const T& Value)
        {
            if constexpr (std::is_same_v<T, bool>) { CastFieldChecked<FBoolProperty>(P)->SetPropertyValue(Address, Value); }
            else if constexpr (std::is_pointer_v<T> && std::is_convertible_v<T, UObject*>)
            {
                const auto* Object = CastFieldChecked<FObjectPropertyBase>(P);
                UObject* ObjectValue = Value;
                if (ObjectValue && !ObjectValue->IsA(Object->PropertyClass)) { return false; }
                const UClass* MetaClass = nullptr;
                if (const auto* Class = CastField<FClassProperty>(P)) { MetaClass = Class->MetaClass; }
                if (const auto* Class = CastField<FSoftClassProperty>(P)) { MetaClass = Class->MetaClass; }
                if (ObjectValue && MetaClass)
                {
                    const UClass* Class = Cast<UClass>(ObjectValue);
                    if (!Class || !Class->IsChildOf(MetaClass)) { return false; }
                }
                Object->SetObjectPropertyValue(Address, ObjectValue);
            }
            else if constexpr (std::is_enum_v<T>)
            {
                const auto* Enum = CastField<FEnumProperty>(P);
                const FNumericProperty* Number = Enum ? Enum->GetUnderlyingProperty() : CastFieldChecked<FByteProperty>(P);
                if (!StaticEnum<T>()->IsValidEnumValueOrBitfield(static_cast<int64>(Value))) { return false; }
                Number->SetIntPropertyValue(Address, static_cast<int64>(Value));
            }
            else { P->CopyCompleteValue(Address, &Value); }
            return true;
        }
    };

    template<class T> struct TValue<TArray<T>>
    {
        static bool Matches(const FProperty* P)
        { const auto* A = CastField<FArrayProperty>(P); return A && A->ArrayDim == 1 && TValue<T>::Matches(A->Inner); }
        static bool Read(const FProperty* P, const void* Address, TArray<T>& Out)
        {
            const auto* Property = CastFieldChecked<FArrayProperty>(P);
            FScriptArrayHelper A(Property, Address);
            Out.SetNum(A.Num());
            for (int32 I = 0; I < A.Num(); ++I) { if (!TValue<T>::Read(Property->Inner, A.GetRawPtr(I), Out[I])) { return false; } }
            return true;
        }
        static bool Write(const FProperty* P, void* Address, const TArray<T>& Value)
        {
            const auto* Property = CastFieldChecked<FArrayProperty>(P);
            FScriptArrayHelper A(Property, Address);
            A.Resize(Value.Num());
            for (int32 I = 0; I < Value.Num(); ++I) { if (!TValue<T>::Write(Property->Inner, A.GetRawPtr(I), Value[I])) { return false; } }
            return true;
        }
    };

    template<class T> struct TValue<TSet<T>>
    {
        static bool Matches(const FProperty* P)
        { const auto* S = CastField<FSetProperty>(P); return S && S->ArrayDim == 1 && TValue<T>::Matches(S->ElementProp); }
        static bool Read(const FProperty* P, const void* Address, TSet<T>& Out)
        {
            const auto* Property = CastFieldChecked<FSetProperty>(P);
            FScriptSetHelper S(Property, Address);
            for (int32 I = 0; I < S.GetMaxIndex(); ++I)
            {
                if (!S.IsValidIndex(I)) { continue; }
                T Item{};
                if (!TValue<T>::Read(Property->ElementProp, S.GetElementPtr(I), Item)) { return false; }
                Out.Add(MoveTemp(Item));
            }
            return true;
        }
        static bool Write(const FProperty* P, void* Address, const TSet<T>& Value)
        {
            const auto* Property = CastFieldChecked<FSetProperty>(P);
            FScriptSetHelper S(Property, Address);
            S.EmptyElements(Value.Num());
            for (const T& Item : Value)
            {
                if (!TValue<T>::Write(Property->ElementProp, S.GetElementPtr(S.AddDefaultValue_Invalid_NeedsRehash()), Item)) { return false; }
            }
            S.Rehash();
            return true;
        }
    };

    template<class K, class V> struct TValue<TMap<K, V>>
    {
        static bool Matches(const FProperty* P)
        {
            const auto* M = CastField<FMapProperty>(P);
            return M && M->ArrayDim == 1 && TValue<K>::Matches(M->KeyProp) && TValue<V>::Matches(M->ValueProp);
        }
        static bool Read(const FProperty* P, const void* Address, TMap<K, V>& Out)
        {
            const auto* Property = CastFieldChecked<FMapProperty>(P);
            FScriptMapHelper M(Property, Address);
            for (int32 I = 0; I < M.GetMaxIndex(); ++I)
            {
                if (!M.IsValidIndex(I)) { continue; }
                K Key{}; V Value{};
                if (!TValue<K>::Read(Property->KeyProp, M.GetKeyPtr(I), Key) || !TValue<V>::Read(Property->ValueProp, M.GetValuePtr(I), Value)) { return false; }
                Out.Add(MoveTemp(Key), MoveTemp(Value));
            }
            return true;
        }
        static bool Write(const FProperty* P, void* Address, const TMap<K, V>& Value)
        {
            const auto* Property = CastFieldChecked<FMapProperty>(P);
            FScriptMapHelper M(Property, Address);
            M.EmptyValues(Value.Num());
            for (const auto& Pair : Value)
            {
                const int32 I = M.AddDefaultValue_Invalid_NeedsRehash();
                if (!TValue<K>::Write(Property->KeyProp, M.GetKeyPtr(I), Pair.Key) || !TValue<V>::Write(Property->ValueProp, M.GetValuePtr(I), Pair.Value)) { return false; }
            }
            M.Rehash();
            return true;
        }
    };
}

class FSuzieFunctionContext;

/** A checked, invocation-scoped view. Never retain this view after its function returns. */
class SUZIE_API FSuzieReflectionValue
{
public:
    template<class T> T Get() const
    {
        T Result{};
        if (!CheckType(SuzieReflection::TValue<T>::Matches(Property))) { return Result; }
        if (!SuzieReflection::TValue<T>::Read(Property, Address, Result)) { Fail(TEXT("Could not read reflected value")); }
        return Result;
    }
    template<class T> bool Set(const T& Value) const
    {
        if (!CanWrite() || !CheckType(SuzieReflection::TValue<T>::Matches(Property))) { return false; }
        // Stage container/object conversions separately, so rejected values cannot leave a partial write.
        void* Temporary = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
        Property->InitializeValue(Temporary);
        const bool bWritten = SuzieReflection::TValue<T>::Write(Property, Temporary, Value);
        if (bWritten) { Property->CopyCompleteValue(Address, Temporary); }
        Property->DestroyValue(Temporary);
        FMemory::Free(Temporary);
        if (!bWritten) { Fail(TEXT("Value is incompatible with the reflected property's declared type")); }
        return bWritten;
    }
    FSuzieReflectionValue Field(FName Name) const;
    FSuzieReflectionValue Element(int32 Index) const;
    int32 Num() const;
    int64 GetEnum() const;
    bool SetEnum(int64 Value) const;
    bool SetEnum(FName Enumerator) const;
    bool IsValid() const { return Property && Address; }

private:
    friend class FSuzieFunctionContext;
    FSuzieReflectionValue(FSuzieFunctionContext& InContext, FProperty* InProperty, void* InAddress, bool bInWritable);
    bool CheckType(bool bMatches) const;
    bool CanWrite() const;
    void Fail(const FString& Message) const;
    FSuzieFunctionContext* Context;
    FProperty* Property;
    void* Address;
    bool bWritable;
};
