#pragma once

#include <memory>
#include <cstdarg>
#include <type_traits>

#define SPDLOG_WCHAR_TO_UTF8_SUPPORT

#include <spdlog/async.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <fmt/printf.h>

namespace spdlog
{
class logger;
}

static spdlog::level::level_enum gSpdLogLevelMap[] = {
	spdlog::level::critical,
	spdlog::level::err,
	spdlog::level::warn,
	spdlog::level::info,
	spdlog::level::trace,
	spdlog::level::debug,
};

class DebugLog
{
public:
	enum LogLevel
	{
		kLevel_FatalError = 0,
		kLevel_Error,
		kLevel_Warning,
		kLevel_Message,
		kLevel_VerboseMessage,
		kLevel_DebugMessage,
		kLevel_None,
	};

	void OpenRelative(int folderID, const char* relPath);

	void SetLogLevel(LogLevel logLevel);

	template<typename ...T>
	void Log(LogLevel level, const char* fmt, const T&... args)
	{
		std::string str;
		if (m_indentLevel) {
			for (int32_t i = 0; i < m_indentLevel; ++i)
				str += "\t";
		}
		try {
			str += fmt::sprintf(fmt, LogArg(args)...);
		} catch (const fmt::format_error&) {
			// A mismatched specifier throws here and game code cannot unwind it, so keep the raw format string instead.
			str += fmt;
		}
		if (m_logger) {
			m_logger->log(gSpdLogLevelMap[level], str);
		}
	}

	void Indent() { m_indentLevel++; }
	void Outdent() { if (m_indentLevel) m_indentLevel--; }

private:
	// fmt::sprintf takes only strings and void pointers, this maps the game types and raw pointers callers pass to it.
	template<typename T>
	static decltype(auto) LogArg(const T& arg)
	{
		if constexpr (std::is_convertible_v<const T&, const char*>)
			return static_cast<const char*>(arg);
		else if constexpr (std::is_pointer_v<T>)
			return static_cast<const void*>(arg);
		else
			return (arg);
	}

	std::shared_ptr<spdlog::logger> m_logger;
	int32_t m_indentLevel = 0;
};

extern DebugLog gLog;

template<typename ...T>
inline void _FATALERROR(const char* fmt, const T&... args)
{
	gLog.Log(DebugLog::kLevel_FatalError, fmt, args...);
}

template<typename ...T>
inline void _ERROR(const char* fmt, const T&... args)
{
	gLog.Log(DebugLog::kLevel_Error, fmt, args...);
}

template<typename ...T>
inline void _WARNING(const char* fmt, const T&... args)
{
	gLog.Log(DebugLog::kLevel_Warning, fmt, args...);
}

template<typename ...T>
inline void _MESSAGE(const char* fmt, const T&... args)
{
	gLog.Log(DebugLog::kLevel_Message, fmt, args...);
}

template<typename ...T>
inline void _VMESSAGE(const char* fmt, const T&... args)
{
	gLog.Log(DebugLog::kLevel_VerboseMessage, fmt, args...);
}

template<typename ...T>
inline void _DMESSAGE(const char* fmt, const T&... args)
{
	gLog.Log(DebugLog::kLevel_DebugMessage, fmt, args...);
}
