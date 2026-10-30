#pragma once

#include <functional>

class TaskDelegate;

// SKSE also drains its AddTask queue from the Job_Post_process worker, so AddTask does not guarantee
// the main thread. Everything that touches the scene graph goes through this queue instead.
namespace MainThreadTasks
{
	enum class Kind : UInt32
	{
		ApplyMorphs,
		ApplyMorphsAttaching,
		UpdateMorphs,
		OverlayVisibility
	};

	bool IsMainThread();

	void Add(TaskDelegate * task);
	void Add(std::function<void()> task);
	void AddDelayed(UInt32 milliseconds, std::function<void()> task);

	// Replaces a pending task with the same key, so repeated requests only do the work once
	void AddCoalesced(Kind kind, UInt32 formId, const void * object, std::function<void()> task);

	void InstallHooks();
}
