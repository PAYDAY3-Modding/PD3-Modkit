#pragma once

#include "SuzieFunctionContext.h"
#include "UObject/Stack.h"

namespace SuzieNative
{
    enum class EScope { Param, Property, Return };
    using FBody = void (*)(FSuzieFunctionContext&);
    using FValidation = bool (*)(UFunction&, FString&);

    // Called by generated wrappers only. No UClass native-name table is modified.
    void Register(const TCHAR* Path, FNativeFuncPtr Thunk, FBody Body, FValidation Validate, const TCHAR* Source);
    void Bind(UFunction& Function);
    void BindAvailable();
    void FinishBindings();
    void Reset();
    void Invoke(const TCHAR* Path, UObject* Self, FFrame& Stack, void* Result);

    template<class T> bool Expect(UFunction& Function, EScope Scope, FName Name, bool bWrite, FString& Error)
    {
        FProperty* Property = Scope == EScope::Return ? Function.GetReturnProperty() :
            FindFProperty<FProperty>(Scope == EScope::Property ? static_cast<UStruct*>(Function.GetOuterUClass()) : &Function, Name);
        const bool bParameter = Scope != EScope::Param || (Property && Property->HasAnyPropertyFlags(CPF_Parm) && !Property->HasAnyPropertyFlags(CPF_ReturnParm));
        const bool bConst = bWrite && ((Scope == EScope::Property && Function.HasAnyFunctionFlags(FUNC_Const)) ||
            (Scope == EScope::Param && Property && Property->HasAnyPropertyFlags(CPF_ConstParm)));
        if (!bParameter || bConst || !SuzieReflection::TValue<T>::Matches(Property))
        {
            Error = FString::Printf(TEXT("Body expects a %s compatible C++ type for %s '%s'; reflected type is %s%s"),
                bWrite ? TEXT("writable") : TEXT("readable"), Scope == EScope::Property ? TEXT("property") : TEXT("parameter/return"),
                *Name.ToString(), Property ? *Property->GetCPPType() : TEXT("<missing>"), bConst ? TEXT(" (const)") : TEXT(""));
            return false;
        }
        return true;
    }
}

void RegisterGeneratedSuzieNativeFunctions();
