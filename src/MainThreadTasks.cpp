#include "MainThreadTasks.h"

#include "skse64/GameAPI.h"
#include "skse64/GameThreads.h"
#include "skse64_common/BranchTrampoline.h"
#include "skse64_common/Relocation.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace MainThreadTasks
{
	namespace Addresses
	{
		static const uintptr_t Main_Update_Call1 = 0x0063E970 + 0x160;	// 36544
		static const uintptr_t Main_Update_Call2 = 0x00641AD0 + 0x11F;	// 36550
	}

	struct Key
	{
		Kind		kind;
		UInt32		formId;
		const void	* object;

		bool operator==(const Key & rhs) const = default;
	};

	struct KeyHash
	{
		size_t operator()(const Key & key) const
		{
			return std::hash<const void *>()(key.object) ^ (size_t(key.formId) << 2) ^ size_t(key.kind);
		}
	};

	struct DelayedTask
	{
		ULONGLONG				due;
		std::function<void()>	run;
	};

	static std::mutex								s_lock;
	static std::vector<std::function<void()>>		s_tasks;
	static std::unordered_map<Key, size_t, KeyHash>	s_coalesced;
	static std::vector<DelayedTask>					s_delayed;

	bool IsMainThread()
	{
		Main * main = *g_main;
		return !main || main->threadId == GetCurrentThreadId();
	}

	void Add(TaskDelegate * task)
	{
		Add([task]()
		{
			task->Run();
			task->Dispose();
		});
	}

	void Add(std::function<void()> task)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_tasks.push_back(std::move(task));
	}

	void AddDelayed(UInt32 milliseconds, std::function<void()> task)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_delayed.push_back({ GetTickCount64() + milliseconds, std::move(task) });
	}

	void AddCoalesced(Kind kind, UInt32 formId, const void * object, std::function<void()> task)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		auto [it, inserted] = s_coalesced.try_emplace(Key{ kind, formId, object }, s_tasks.size());
		if (inserted)
			s_tasks.push_back(std::move(task));
		else
			s_tasks[it->second] = std::move(task);
	}

	static void Drain()
	{
		std::vector<std::function<void()>> tasks;
		{
			std::lock_guard<std::mutex> lock(s_lock);
			tasks.swap(s_tasks);
			s_coalesced.clear();

			ULONGLONG now = GetTickCount64();
			for (auto it = s_delayed.begin(); it != s_delayed.end();)
			{
				if (it->due <= now)
				{
					tasks.push_back(std::move(it->run));
					it = s_delayed.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		for (auto & task : tasks)
			task();
	}

	typedef void(*_MainUpdate)(void *, void *, void *, void *);

	// Each call site keeps its own original, another plugin may already have hooked either one
	template<int Site>
	struct MainUpdateHook
	{
		static inline _MainUpdate original = nullptr;

		static void Hooked(void * main, void * arg2, void * arg3, void * arg4)
		{
			original(main, arg2, arg3, arg4);
			Drain();
		}

		static void Install(uintptr_t address)
		{
			RelocAddr<uintptr_t> site(address);
			uintptr_t call = site.GetUIntPtr();
			original = (_MainUpdate)(call + 5 + *reinterpret_cast<SInt32 *>(call + 1));
			g_branchTrampoline.Write5Call(call, (uintptr_t)Hooked);
		}
	};

	void InstallHooks()
	{
		MainUpdateHook<1>::Install(Addresses::Main_Update_Call1);
		MainUpdateHook<2>::Install(Addresses::Main_Update_Call2);
	}
}
