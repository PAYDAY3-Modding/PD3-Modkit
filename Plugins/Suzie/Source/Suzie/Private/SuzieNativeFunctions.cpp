#include "SuzieNativeFunctions.h"
#include "SuziePlugin.h"
#include "UObject/StrongObjectPtr.h"

namespace SuzieNative
{
    namespace
    {
        struct FEntry
        {
            FNativeFuncPtr Thunk = nullptr;
            FBody Body = nullptr;
            FValidation Validate = nullptr;
            FString Source;
            TWeakObjectPtr<UFunction> Function;
            FNativeFuncPtr Previous = nullptr;
            bool bRejected = false;
            bool bReported = false;
        };
        TMap<FString, FEntry>& Entries() { static TMap<FString, FEntry> Values; return Values; }

        bool Supported(const FProperty& P)
        {
            if (P.ArrayDim != 1) { return false; }
            if (const auto* A = CastField<FArrayProperty>(&P)) { return Supported(*A->Inner); }
            if (const auto* S = CastField<FSetProperty>(&P)) { return Supported(*S->ElementProp); }
            if (const auto* M = CastField<FMapProperty>(&P)) { return Supported(*M->KeyProp) && Supported(*M->ValueProp); }
            return P.IsA<FNumericProperty>() || P.IsA<FBoolProperty>() || P.IsA<FEnumProperty>() || P.IsA<FObjectPropertyBase>() ||
                P.IsA<FStructProperty>() || P.IsA<FStrProperty>() || P.IsA<FNameProperty>() || P.IsA<FTextProperty>();
        }

        // Two bool properties of the same reflected type may use different packed bit masks.
        void CopyValue(const FProperty& Source, const void* From, const FProperty& Destination, void* To)
        {
            if (const auto* B = CastField<FBoolProperty>(&Source))
            { CastFieldChecked<FBoolProperty>(&Destination)->SetPropertyValue(To, B->GetPropertyValue(From)); }
            else { Destination.CopyCompleteValue(To, From); }
        }

        void Unbind(FEntry& Entry)
        {
            if (UFunction* Function = Entry.Function.Get(); Function && Function->GetNativeFunc() == Entry.Thunk)
            {
                Function->SetNativeFunc(Entry.Previous);
                Function->RemoveMetaData(TEXT("SuzieNativeBody"));
            }
            Entry.Function.Reset();
        }
    }

    void Register(const TCHAR* Path, FNativeFuncPtr Thunk, FBody Body, FValidation Validate, const TCHAR* Source)
    {
        if (!Path || !FString(Path).StartsWith(TEXT("/Script/")) || !FString(Path).Contains(TEXT(":")) || !Thunk || !Body || !Validate)
        {
            UE_LOG(LogSuzie, Error, TEXT("Invalid native body registration: %s (%s)"), Path ? Path : TEXT("<null>"), Source ? Source : TEXT("unknown source"));
            return;
        }
        if (FEntry* Existing = Entries().Find(Path))
        {
            Unbind(*Existing);
            Existing->bRejected = true;
            Existing->bReported = true;
            UE_LOG(LogSuzie, Error, TEXT("Duplicate native body for %s: %s and %s; neither will bind"), Path, *Existing->Source, Source);
            return;
        }
        FEntry Entry;
        Entry.Thunk = Thunk; Entry.Body = Body; Entry.Validate = Validate; Entry.Source = Source;
        Entries().Add(Path, MoveTemp(Entry));
    }

    void Bind(UFunction& Function)
    {
        FEntry* Entry = Entries().Find(Function.GetPathName());
        if (!Entry || Entry->bRejected || Entry->Function.Get() == &Function) { return; }
        FString Error;
        if (!Function.GetOuter()->IsA<UClass>() || !Function.HasMetaData(TEXT("SuzieSyntheticNative")) ||
            !Function.HasAnyFunctionFlags(FUNC_Native) || Function.HasAnyFunctionFlags(FUNC_Net | FUNC_Delegate | FUNC_MulticastDelegate) || Function.HasMetaData(TEXT("Latent")))
        { Error = TEXT("Only Suzie-generated synchronous native class functions can receive bodies (no RPCs, delegates or latent functions)"); }
        for (TFieldIterator<FProperty> It(&Function, EFieldIterationFlags::None); Error.IsEmpty() && It; ++It)
        {
            if (!It->HasAnyPropertyFlags(CPF_Parm) || !Supported(**It) || It->GetOffset_ForUFunction() < 0 ||
                It->GetOffset_ForUFunction() + It->GetSize() > Function.ParmsSize)
            { Error = FString::Printf(TEXT("Unsupported or invalid parameter layout: %s"), *It->GetName()); }
        }
        if (Error.IsEmpty() && !Entry->Validate(Function, Error) && Error.IsEmpty()) { Error = TEXT("Body signature validation failed"); }
        if (!Error.IsEmpty())
        {
            Entry->bReported = true;
            Entry->bRejected = true;
            UE_LOG(LogSuzie, Error, TEXT("Cannot bind native body %s (%s): %s"), *Function.GetPathName(), *Entry->Source, *Error);
            return;
        }
        Unbind(*Entry);
        Entry->Function = &Function;
        Entry->Previous = Function.GetNativeFunc();
        Function.SetNativeFunc(Entry->Thunk);
        Function.SetMetaData(TEXT("SuzieNativeBody"), *Entry->Source);
        UE_LOG(LogSuzie, Display, TEXT("Bound native body %s from %s"), *Function.GetPathName(), *Entry->Source);
    }

    void FinishBindings()
    {
        BindAvailable();
        for (auto& Pair : Entries())
        {
            if (!Pair.Value.Function.IsValid() && !Pair.Value.bReported)
            {
                Pair.Value.bReported = true;
                UE_LOG(LogSuzie, Error, TEXT("Native body target not found: %s (%s). Use the declaring class's reflected identity."), *Pair.Key, *Pair.Value.Source);
            }
        }
    }

    void BindAvailable()
    {
        for (auto& Pair : Entries())
        {
            if (UFunction* Function = FindObject<UFunction>(nullptr, *Pair.Key)) { Bind(*Function); }
        }
    }

    void Reset()
    {
        for (auto& Pair : Entries()) { Unbind(Pair.Value); }
        Entries().Empty();
    }

    void Invoke(const TCHAR* Path, UObject* Self, FFrame& Stack, void* Result)
    {
        const FEntry* Entry = Entries().Find(Path);
        UFunction* Function = Entry ? Entry->Function.Get() : nullptr;
        // Static native thunks do not require the incoming ProcessEvent context to be an
        // instance of their declaring class. Always expose that class's CDO to the body.
        if (Function && IsInGameThread() && Function->HasAnyFunctionFlags(FUNC_Static))
        { Self = Function->GetOuterUClass()->GetDefaultObject(); }
        if (!Function || !Self || !Self->IsA(Function->GetOuterUClass()) || !IsInGameThread())
        {
            UE_LOG(LogSuzie, Error, TEXT("Invalid or non-game-thread invocation of native body %s"), Path);
            return;
        }
        TStrongObjectPtr<UObject> Caller(Stack.Object);
        FSuzieFunctionContext Context(Self, *Function);
        struct FOutput { FProperty* Parameter; FProperty* DestinationType; void* Destination; };
        TArray<FOutput, TInlineAllocator<8>> Outputs;
        const bool bBytecode = Stack.Code != nullptr;
        for (TFieldIterator<FProperty> It(Function, EFieldIterationFlags::None); It; ++It)
        {
            FProperty* P = *It;
            if (!P->HasAnyPropertyFlags(CPF_Parm) || P->HasAnyPropertyFlags(CPF_ReturnParm)) { continue; }
            void* Local = P->ContainerPtrToValuePtr<void>(Context.GetParameterMemory());
            void* Source = nullptr;
            FProperty* SourceType = P;
            if (bBytecode)
            {
                if (*Stack.Code == EX_EndFunctionParms)
                {
                    Context.Fail(FString::Printf(TEXT("Missing VM argument '%s'"), *P->GetName()));
                    break;
                }
                Stack.MostRecentProperty = nullptr;
                Stack.MostRecentPropertyAddress = nullptr;
                Stack.MostRecentPropertyContainer = nullptr;
                if (auto* Bool = CastField<FBoolProperty>(P))
                {
                    bool Value = false;
                    Stack.StepCompiledIn(&Value, P->GetClass());
                    Bool->SetPropertyValue(Local, Value);
                }
                else
                {
                    // VM and property storage can differ (notably object-pointer containers).
                    void* Scratch = FMemory::Malloc(P->GetSize(), P->GetMinAlignment());
                    P->InitializeValue(Scratch);
                    Stack.StepCompiledIn(Scratch, P->GetClass());
                    P->CopyCompleteValueFromScriptVM(Local, Scratch);
                    P->DestroyValue(Scratch);
                    FMemory::Free(Scratch);
                }
                if (P->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm) && Stack.MostRecentPropertyAddress)
                {
                    Source = Stack.MostRecentPropertyAddress;
                    SourceType = Stack.MostRecentProperty;
                    if (!SourceType || !P->SameType(SourceType))
                    { Context.Fail(FString::Printf(TEXT("VM reference type mismatch for '%s'"), *P->GetName())); Source = nullptr; }
                }
            }
            else
            {
                if (Stack.Node != Function || Stack.PropertyChainForCompiledIn != P)
                { Context.Fail(TEXT("Compiled-in frame does not match the bound function signature")); break; }
                Stack.PropertyChainForCompiledIn = P->Next;
                if (P->HasAnyPropertyFlags(CPF_OutParm))
                {
                    for (FOutParmRec* Out = Stack.OutParms; Out; Out = Out->NextOutParm)
                    { if (Out->Property == P) { Source = Out->PropAddr; break; } }
                }
                else if (Stack.Locals) { Source = P->ContainerPtrToValuePtr<void>(Stack.Locals); }
                if (!Source) { Context.Fail(FString::Printf(TEXT("Missing compiled-in parameter storage for '%s'"), *P->GetName())); break; }
            }
            if (Source)
            {
                CopyValue(*SourceType, Source, *P, Local);
                if (P->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
                {
                    for (const FOutput& Other : Outputs)
                    { if (Other.Destination == Source && P->SameType(Other.Parameter)) { Context.AliasParameter(P, Other.Parameter); break; } }
                    Outputs.Add({ P, SourceType, Source });
                }
            }
        }
        if (bBytecode)
        {
            if (*Stack.Code == EX_EndFunctionParms) { ++Stack.Code; }
            else { Context.Fail(TEXT("VM argument stream does not end at EX_EndFunctionParms; body was not executed")); }
        }
        static thread_local int32 Depth = 0;
        TGuardValue<int32> DepthGuard(Depth, Depth + 1);
        if (Depth > 128) { Context.Fail(TEXT("Native body recursion limit exceeded")); }
        if (Context.IsValid()) { Entry->Body(Context); }
        if (Context.IsValid())
        {
            for (const FOutput& Out : Outputs)
            {
                if (Out.Parameter->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm) && !Out.Parameter->HasAnyPropertyFlags(CPF_ConstParm))
                {
                    FProperty* Source = Context.ResolveParameterAlias(Out.Parameter);
                    CopyValue(*Source, Source->ContainerPtrToValuePtr<void>(Context.GetParameterMemory()), *Out.DestinationType, Out.Destination);
                }
            }
        }
        if (FProperty* Return = Function->GetReturnProperty(); Return && Result)
        {
            void* Value = Return->ContainerPtrToValuePtr<void>(Context.GetParameterMemory());
            if (!Context.IsValid()) { Return->ClearValue(Value); }
            if (bBytecode)
            {
                if (const auto* Bool = CastField<FBoolProperty>(Return)) { *static_cast<bool*>(Result) = Bool->GetPropertyValue(Value); }
                else { Return->CopyCompleteValueToScriptVM(Result, Value); }
            }
            else { Return->CopyCompleteValue(Result, Value); }
        }
    }
}
