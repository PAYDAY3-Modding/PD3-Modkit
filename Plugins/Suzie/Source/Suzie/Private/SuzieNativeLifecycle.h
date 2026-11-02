#pragma once

#include "SuzieFunctionContext.h"

namespace SuzieNativeLifecycle
{
    using FBody = void (*)(FSuzieFunctionContext&);
    void Register(const TCHAR* ClassPath, ESuzieCallbackPhase Phase, FName Property, FBody Body, const TCHAR* Source);
    void Start();
    void InitializeClass(UObject& Object, const UClass& DeclaringClass);
    void FinishBindings();
    void Reset();
    void WatchDependency(UObject& Owner, UObject* Dependency, FName Property);
    void ClearDependencies(UObject& Owner);
    void RequestRefresh(UObject& Object);
    bool ObserveWidget(UObject& Object);
}
