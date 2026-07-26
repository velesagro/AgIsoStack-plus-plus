//================================================================================================
/// @file can_stack_logger.cpp
///
/// @brief A class that acts as a logging sink. The intent is that someone could make their own
/// derived class of logger and inject it into the CAN stack to get helpful debug logging.
/// @author Adrian Del Grosso
///
/// @copyright 2022 The Open-Agriculture Developers
//================================================================================================
#include "isobus/isobus/can_stack_logger.hpp"

#include <cstdarg>
#include <cstdio>
#include <iostream>

namespace isobus
{
	CANStackLogger *CANStackLogger::logger = nullptr;
	CANStackLogger::LoggingLevel CANStackLogger::currentLogLevel = LoggingLevel::Info;
	Mutex CANStackLogger::loggerMutex;

#ifndef DISABLE_CAN_STACK_LOGGER

#ifdef CAN_STACK_LOGGER_NO_HEAP
#ifndef CAN_STACK_LOGGER_LINE_MAX
#define CAN_STACK_LOGGER_LINE_MAX 160
#endif
	void CANStackLogger::CAN_stack_log_lean(LoggingLevel level, const char *format, ...)
	{
		// Швидкий вихід ДО взяття м'ютекса й ДО будь-якого форматування:
		// вимкнені рівні (Debug/Info при порозі Warning) коштують ~декілька
		// інструкцій. Читання без локу race-benign: обидва поля — прості
		// скаляри, найгірший наслідок — один пропущений/зайвий рядок на межі
		// зміни рівня.
		if ((nullptr == logger) || (level < currentLogLevel))
		{
			return;
		}

		LOCK_GUARD(Mutex, loggerMutex);
		CANStackLogger *canStackLogger = nullptr;

		if ((get_can_stack_logger(canStackLogger)) &&
		    (level >= get_log_level()))
		{
			// Один буфер на весь стек: захищений loggerMutex, тому static
			// безпечний. Живе у .bss (SRAM), НЕ в купі й НЕ на стеку задачі.
			static char leanBuffer[CAN_STACK_LOGGER_LINE_MAX];

			va_list args;
			va_start(args, format);
			vsnprintf(leanBuffer, sizeof(leanBuffer), format, args);
			va_end(args);

			canStackLogger->sink_CAN_stack_log_lean(level, leanBuffer);
		}
	}

	void CANStackLogger::CAN_stack_log(LoggingLevel level, const std::string &logText)
	{
		// Рідкісний шлях (єдиний call site у бібліотеці — oss.str() у
		// time_date_interface): рядок уже сконструйований викликачем,
		// додаткових алокацій тут немає.
		CAN_stack_log_lean(level, "%s", logText.c_str());
	}
#else
	void CANStackLogger::CAN_stack_log(LoggingLevel level, const std::string &logText)
	{
		LOCK_GUARD(Mutex, loggerMutex);
		CANStackLogger *canStackLogger = nullptr;

		if ((get_can_stack_logger(canStackLogger)) &&
		    (level >= get_log_level()))
		{
			canStackLogger->sink_CAN_stack_log(level, logText);
		}
	}
#endif // CAN_STACK_LOGGER_NO_HEAP

	void CANStackLogger::debug(const std::string &logText)
	{
		CAN_stack_log(LoggingLevel::Debug, logText);
	}

	void CANStackLogger::info(const std::string &logText)
	{
		CAN_stack_log(LoggingLevel::Info, logText);
	}

	void CANStackLogger::warn(const std::string &logText)
	{
		CAN_stack_log(LoggingLevel::Warning, logText);
	}

	void CANStackLogger::error(const std::string &logText)
	{
		CAN_stack_log(LoggingLevel::Error, logText);
	}

	void CANStackLogger::critical(const std::string &logText)
	{
		CAN_stack_log(LoggingLevel::Critical, logText);
	}

#endif // DISABLE_CAN_STACK_LOGGER

	void CANStackLogger::set_can_stack_logger_sink(CANStackLogger *logSink)
	{
		logger = logSink;
	}

	CANStackLogger::LoggingLevel CANStackLogger::get_log_level()
	{
		return currentLogLevel;
	}

	void CANStackLogger::set_log_level(LoggingLevel newLogLevel)
	{
		currentLogLevel = newLogLevel;
	}

	void CANStackLogger::sink_CAN_stack_log(LoggingLevel, const std::string &)
	{
		// Override this function to use the log sink
	}

	void CANStackLogger::sink_CAN_stack_log_lean(LoggingLevel level, const char *logText)
	{
#ifdef CAN_STACK_LOGGER_NO_HEAP
		// В embedded-режимі дефолт — нічого: sink зобов'язаний перевизначити
		// саме lean-варіант, інакше форвардинг у std::string звів би нанівець
		// увесь сенс no-heap шляху.
		(void)level;
		(void)logText;
#else
		// На десктопі — сумісність: форвардимо у штатний string-sink, щоб
		// наявні sink-и (console logger тощо) працювали без змін.
		sink_CAN_stack_log(level, std::string(logText));
#endif
	}

	bool CANStackLogger::get_can_stack_logger(CANStackLogger *&canStackLogger)
	{
		canStackLogger = logger;
		return (nullptr != canStackLogger);
	}
} // namespace isobus
