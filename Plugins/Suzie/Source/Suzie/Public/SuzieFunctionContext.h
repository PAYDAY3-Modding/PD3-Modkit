#pragma once

#include "SuzieReflectionValue.h"
#include "UObject/GCObject.h"
#include "UObject/StructOnScope.h"

enum class ESuzieCallbackPhase : uint8 { Function, Construct, PostInitialize, PropertyChanged, Refresh };

/** Reflection access for native bodies and editor lifecycle callbacks. Values are copies. */
class SUZIE_API FSuzieFunctionContext final : public FGCObject
{
public:
    FSuzieFunctionContext(UObject* InSelf, UFunction& InFunction, bool bInSetup = false);
    /** Property-only lifecycle scope; no parameter frame or native virtual override.
     * Construct runs before final defaults, on the construction thread, scoped to its declaring
     * Suzie class (Blueprint fields are not initialized yet). PostInitialize is deferred to the
     * editor game thread after defaults/PostLoad, including CDOs; it is not actor BeginPlay. */
    explicit FSuzieFunctionContext(UObject* InSelf, ESuzieCallbackPhase InPhase = ESuzieCallbackPhase::Refresh,
        FName InChangedProperty = NAME_None, FName InChangedLeaf = NAME_None, const UClass* InPropertyScope = nullptr);
    UObject* GetSelf() const { return Self.Get(); }
    UFunction& GetFunction() const { checkf(Function, TEXT("Lifecycle contexts have no UFunction")); return *Function; }
    bool IsValid() const { return Error.IsEmpty(); }
    const FString& GetError() const { return Error; }
    ESuzieCallbackPhase GetPhase() const { return Phase; }
    bool IsClassDefaultObject() const { return Self && Self->HasAnyFlags(RF_ClassDefaultObject); }
    bool IsTemplate() const { return Self && Self->IsTemplate(); }
    FName GetChangedPropertyName() const { return ChangedProperty; }
    FName GetChangedLeafPropertyName() const { return ChangedLeaf; }

    /** Editor-only weak subscriptions. NAME_None watches any property. ClearDependencies
     * replaces the complete dependency set for this object, not just this callback. */
    void WatchDependency(UObject* Dependency, FName Property = NAME_None);
    void ClearDependencies();
    void RequestRefresh();
    /** Observe weak Slate-widget replacement; deferred refresh, not a native virtual override. */
    void ObserveWidget();

    FSuzieReflectionValue Param(FName Name);
    FSuzieReflectionValue Property(FName Name, UObject* Owner = nullptr);
    FSuzieReflectionValue Return();
    template<class T> T GetParam(FName Name) { return Param(Name).Get<T>(); }
    template<class T> bool SetParam(FName Name, const T& Value) { return Param(Name).Set<T>(Value); }
    template<class T> T GetProperty(FName Name) { return Property(Name).Get<T>(); }
    template<class T> bool SetProperty(FName Name, const T& Value) { return Property(Name).Set<T>(Value); }
    template<class T> T GetReturn() { return Return().Get<T>(); }
    template<class T> bool SetReturn(const T& Value) { return Return().Set<T>(Value); }

    /** Setup inputs, call synchronously, then inspect the returned context for return/out values.
     * Latent, delegate and RPC calls are deliberately unsupported. No runtime code is shipped with mods. */
    TUniquePtr<FSuzieFunctionContext> CallFunction(FName Name,
        TFunction<void(FSuzieFunctionContext&)> Setup = {}, UObject* Target = nullptr);

    // Execution-bridge access. Memory is initialized, reflected and GC-visible for this scope only.
    uint8* GetParameterMemory() { return Parameters.GetStructMemory(); }
    FProperty* ResolveParameterAlias(FProperty* P) const { if (FProperty* const* Found = Aliases.Find(P)) { return *Found; } return P; }
    void AliasParameter(FProperty* P, FProperty* Source) { Aliases.Add(P, ResolveParameterAlias(Source)); }
    void Fail(const FString& Message);
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override { return TEXT("Suzie native function invocation"); }

private:
    TObjectPtr<UObject> Self;
    TObjectPtr<UFunction> Function;
    TObjectPtr<const UClass> PropertyScope;
    FStructOnScope Parameters;
    FString Error;
    TMap<FProperty*, FProperty*> Aliases;
    bool bSetup;
    ESuzieCallbackPhase Phase = ESuzieCallbackPhase::Function;
    FName ChangedProperty;
    FName ChangedLeaf;
};
