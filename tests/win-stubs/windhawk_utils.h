// Minimal Windhawk SDK stub - see README.md in this folder.
#pragma once
#include "windows.h"
namespace WindhawkUtils {
template <typename T>
bool SetFunctionHook(T targetFunction, T hookFunction, T* originalFunction);
}
