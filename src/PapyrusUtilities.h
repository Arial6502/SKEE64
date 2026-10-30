#pragma once

#include "skse64/PapyrusVM.h"

namespace papyrusVM
{
template<typename T>
UInt64 GetObjectHandle(T* src, UInt32 typeID)
{
	VMClassRegistry* registry = (*g_skyrimVM)->GetClassRegistry();
	IObjectHandlePolicy* policy = registry->GetHandlePolicy();

	return policy->Create(typeID, (void*)src);
}

template<typename T>
T* GetObjectFromHandle(UInt64 handle)
{
	VMClassRegistry* registry = (*g_skyrimVM)->GetClassRegistry();
	IObjectHandlePolicy* policy = registry->GetHandlePolicy();
	if (handle == policy->GetInvalidHandle()) {
		return nullptr;
	}

	return static_cast<T*>(policy->Resolve(T::kTypeID, handle));
}
}