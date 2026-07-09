//================================================================================================
/// @file thread_synchronization.hpp
///
/// @brief A single header file to automatically include the correct thread synchronization
/// @author Daan Steenbergen
///
/// @copyright 2024 The Open-Agriculture Developers
//================================================================================================
#ifndef THREAD_SYNCHRONIZATION_HPP
#define THREAD_SYNCHRONIZATION_HPP

#if defined CAN_STACK_DISABLE_THREADS || defined ARDUINO
#include <queue>

namespace isobus
{
	/// @brief A dummy mutex class when threading is disabled.
	class Mutex
	{
	};
	/// @brief A dummy recursive mutex class when threading is disabled.
	class RecursiveMutex
	{
	};

	/// @brief A dummy condition variable for when threading is disabled
	class ConditionVariable
	{
	};

	/// @brief A dummy thread for when threading is disabled
	class Thread
	{
	};
}
/// @brief Disabled LOCK_GUARD macro since threads are disabled.
#define LOCK_GUARD(type, x)

/// @brief A template class for a queue, since threads are disabled this is a simple queue.
/// @tparam T The item type for the queue.
template<typename T>
class LockFreeQueue
{
public:
	/// @brief Constructor for the lock free queue.
	explicit LockFreeQueue(std::size_t) {}

	/// @brief Push an item to the queue.
	/// @param item The item to push to the queue.
	/// @return Simply returns true, since this version of the queue is not limited in size.
	bool push(const T &item)
	{
		queue.push(item);
		return true;
	}

	/// @brief Peek at the next item in the queue.
	/// @param item The item to peek at in the queue.
	/// @return True if the item was peeked at in the queue, false if the queue is empty.
	bool peek(T &item)
	{
		if (queue.empty())
		{
			return false;
		}

		item = queue.front();
		return true;
	}

	/// @brief Pop an item from the queue.
	/// @return True if the item was popped from the queue, false if the queue is empty.
	bool pop()
	{
		if (queue.empty())
		{
			return false;
		}

		queue.pop();
		return true;
	}

	/// @brief Check if the queue is full.
	/// @return Always returns false, since this version of the queue is not limited in size.
	bool is_full() const
	{
		return false;
	}

	/// @brief Clear the queue.
	void clear()
	{
		queue = {};
	}

private:
	std::queue<T> queue; ///< The queue
};

#elif defined USE_CMSIS_RTOS2_THREADING

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <atomic>
#include <cassert>
#include <cstddef> // NULL — do not rely on cmsis_os.h pulling it in transitively
#include <cstdint>
#include <functional>
#include <vector>

namespace isobus
{
	/// @brief A wrapper around a CMSIS RTOS 2 mutex.
	/// @details See definition at https://www.keil.com/pack/doc/CMSIS/RTOS2/html/group__CMSIS__RTOS.html
	class Mutex
	{
	public:
		/// @brief Constructor for the CMSIS RTOS2 mutex wrapper
		Mutex() :
		  handle(nullptr)
		{
		}

		/// @brief Locks the mutex. Part of BasicLockable requirements.
		/// @note lock()/try_lock()/unlock() must all use the SAME scheduler-state
		/// condition. Previously lock() skipped taking the semaphore while the
		/// scheduler was suspended (checked == RUNNING) but unlock() still gave it
		/// (checked != NOT_STARTED), producing an unpaired xSemaphoreGive on a
		/// mutex that was never taken. All three now act only when the scheduler
		/// is RUNNING; pre-kernel and scheduler-suspended contexts are effectively
		/// single-threaded, so skipping the semaphore there is safe.
		void lock()
		{
			if (ready() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
			{
				xSemaphoreTake((SemaphoreHandle_t)handle, portMAX_DELAY);
			}
		}

		/// @brief Attempts to lock the mutex, and doesn't wait if it's not available.
		/// @returns true if the mutex was successfully locked, false otherwise.
		bool try_lock()
		{
			if (ready() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
			{
				return xSemaphoreTake((SemaphoreHandle_t)handle, 0) == pdTRUE;
			}
			return true; // Pre-kernel / scheduler suspended: single-threaded, treat as locked.
		}

		/// @brief Unlocks the mutex. Part of BasicLockable requirements.
		void unlock()
		{
			if (nullptr != handle && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
			{
				xSemaphoreGive((SemaphoreHandle_t)handle);
			}
		}

	protected:
		/// @brief Checks if the mutex is ready to be used. Initializes the mutex if it's not.
		/// @returns true if the mutex is ready to be used, false otherwise.
		virtual bool ready()
		{
			if (nullptr == handle)
			{
				const osMutexAttr_t attributes = {
					nullptr,
#ifdef osCMSIS_FreeRTOS // FreeRTOS doesn't support robust mutexes
					osMutexPrioInherit,
#else
					osMutexPrioInherit | osMutexRobust,
#endif
					nullptr,
					0
				};
				handle = osMutexNew(&attributes);

				// Fallback: якщо CMSIS wrapper відмовив (напр. ядро ще не ready),
				// спробувати FreeRTOS API напряму — воно працює до osKernelStart.
				if (nullptr == handle)
				{
					handle = (osMutexId_t)xSemaphoreCreateMutex();
				}
			}
			return nullptr != handle;
		}
		osMutexId_t handle; ///< Mutex ID for reference by other functions or NULL in case of error or not yet initialized
	};

	/// @brief A wrapper around a CMSIS RTOS 2 recursive mutex.
	/// @details See definition at https://www.keil.com/pack/doc/CMSIS/RTOS2/html/group__CMSIS__RTOS.html
	class RecursiveMutex : public Mutex
	{
	protected:
		/// @brief Checks if the mutex is ready to be used.
		/// Initializes the mutex if it's not.
		/// @returns true if the mutex is ready to be used, false otherwise.
		bool ready() override
		{
			if (nullptr == handle)
			{
				const osMutexAttr_t attributes = {
					nullptr,
#ifdef osCMSIS_FreeRTOS // FreeRTOS doesn't support robust mutexes
					osMutexPrioInherit | osMutexRecursive,
#else
					osMutexPrioInherit | osMutexRobust | osMutexRecursive,
#endif
					nullptr,
					0
				};
				handle = osMutexNew(&attributes);
			}
			return nullptr != handle;
		}
	};

	template<class T>
	/// @brief A class to automatically lock and unlock a mutex when the scope ends.
	/// Meant for systems with no support for std::lock_guard.
	class LockGuard
	{
	public:
		/// @brief Constructor for the LockGuard class.
		/// @param mutex The mutex to lock.
		/// @details Locks the mutex when the scope starts.
		/// Unlocks the mutex when the scope ends.
		LockGuard(T *mutex) :
		  lockable(mutex)
		{
			lockable->lock();
		}

		/// @brief Destructor for the LockGuard class.
		/// @details Unlocks the mutex when the scope ends.
		~LockGuard()
		{
			lockable->unlock();
		}

	private:
		T *lockable; ///< The mutex to lock and unlock.
	};

	/// @brief A class that emulates a condition variable using CMSIS thread flags.
	/// @note Limitation: only a single waiting thread is supported (thread flags
	/// target one thread), which matches how the CAN stack uses it (one update
	/// thread). Thread flags are latched, so a notification that arrives between
	/// unlock() and osThreadFlagsWait() is not lost.
	class ConditionVariable
	{
	public:
		ConditionVariable() = default;

		void wait_for(Mutex &mutex, std::uint32_t timeout)
		{
			targetThread = osThreadGetId();
			// Release the mutex for the duration of the wait, like
			// std::condition_variable::wait_for does. Previously the mutex stayed
			// locked while blocked on the thread flag, so any other thread trying
			// to take it stalled for up to the full timeout.
			mutex.unlock();
			osThreadFlagsWait(0x00000001, osFlagsWaitAny, timeout);
			mutex.lock();
		}

		void notify_all()
		{
			// May legitimately be called before any thread has ever waited
			// (e.g. a CAN frame arrives before the update thread's first wait);
			// osThreadFlagsSet(NULL, ...) is invalid, so guard it.
			if (NULL != targetThread)
			{
				osThreadFlagsSet(targetThread, 0x00000001);
			}
		}

	private:
		osThreadId_t targetThread = NULL;
	};

	class Thread
	{
	public:
		Thread(osThreadFunc_t threadFunction, void *argument)
		{
			osThreadAttr_t attributes = {
				NULL,
				osThreadDetached,
				NULL,
				0,
				NULL,
				0,
				osPriorityNormal,
				0,
				0
			};
			myID = osThreadNew(threadFunction, argument, &attributes);
			if (NULL == myID)
			{
				while (true)
				{
					// If your code is stuck in here, it means you don't have enough OS memory left to spawn the thread.
					// Consider increasing it.
				}
			}
			else
			{
				isJoinable = true;
			}
		}

		bool joinable() const
		{
			return isJoinable;
		}

		void join()
		{
			// WARNING: this is NOT a real join. osThreadTerminate() forcibly kills
			// the thread instead of waiting for it to finish; any mutex it holds
			// stays locked and any resource it owns leaks. CMSIS-FreeRTOS does not
			// reliably implement osThreadJoin(), so callers must ensure the thread
			// has reached a safe point (e.g. its termination flag was observed)
			// before calling join().
			if (isJoinable)
			{
				osThreadTerminate(myID);
			}
		}

	private:
		osThreadId_t myID = NULL;
		bool isJoinable = false;
	};
} // namespace isobus

namespace std
{
	using mutex = isobus::Mutex;
	using recursive_mutex = isobus::RecursiveMutex;
	using condition_variable = isobus::ConditionVariable;
} // namespace std

#define LOCK_GUARD(type, x) const LockGuard<type> x##Lock(&x)

/// @brief A template class for a lock free queue.
/// @tparam T The item type for the queue.
template<typename T>
class LockFreeQueue
{
public:
	/// @brief Constructor for the lock free queue.
	explicit LockFreeQueue(std::size_t size) :
	  buffer(size > 0 ? size : 1), capacity(size > 0 ? size : 1)
	{
		// Validate the size of the queue. The size==0 fallback must be applied in
		// the member initializer list: the previous implementation assigned the
		// local `size` parameter in the constructor body AFTER buffer/capacity had
		// already been constructed with 0, which left capacity==0 and made
		// nextIndex() divide by zero when assertions are disabled (NDEBUG).
		assert(size > 0 && "The size of the queue must be greater than 0.");
	}

	/// @brief Push an item to the queue.
	/// @param item The item to push to the queue.
	/// @return True if the item was pushed to the queue, false if the queue is full.
	bool push(const T &item)
	{
		const auto currentWriteIndex = writeIndex.load(std::memory_order_relaxed);
		const auto nextWriteIndex = nextIndex(currentWriteIndex);

		if (nextWriteIndex == readIndex.load(std::memory_order_acquire))
		{
			// The buffer is full.
			return false;
		}

		buffer[currentWriteIndex] = item;
		writeIndex.store(nextWriteIndex, std::memory_order_release);
		return true;
	}

	/// @brief Peek at the next item in the queue.
	/// @param item The item to peek at in the queue.
	/// @return True if the item was peeked at in the queue, false if the queue is empty.
	bool peek(T &item)
	{
		const auto currentReadIndex = readIndex.load(std::memory_order_relaxed);
		if (currentReadIndex == writeIndex.load(std::memory_order_acquire))
		{
			// The buffer is empty.
			return false;
		}

		item = buffer[currentReadIndex];
		return true;
	}

	/// @brief Pop an item from the queue.
	/// @return True if the item was popped from the queue, false if the queue is empty.
	bool pop()
	{
		const auto currentReadIndex = readIndex.load(std::memory_order_relaxed);
		if (currentReadIndex == writeIndex.load(std::memory_order_acquire))
		{
			// The buffer is empty.
			return false;
		}

		readIndex.store(nextIndex(currentReadIndex), std::memory_order_release);
		return true;
	}

	/// @brief Check if the queue is full.
	/// @return True if the queue is full, false if the queue is not full.
	bool is_full() const
	{
		return nextIndex(writeIndex.load(std::memory_order_acquire)) == readIndex.load(std::memory_order_acquire);
	}

	/// @brief Clear the queue.
	void clear()
	{
		// Simply move the read index to the write index.
		readIndex.store(writeIndex.load(std::memory_order_acquire), std::memory_order_release);
	}

private:
	std::vector<T> buffer; ///< The buffer for the circular buffer.
	std::atomic<std::size_t> readIndex = { 0 }; ///< The read index for the circular buffer.
	std::atomic<std::size_t> writeIndex = { 0 }; ///< The write index for the circular buffer.
	const std::size_t capacity; ///< The capacity of the circular buffer.

	/// @brief Get the next index in the circular buffer.
	/// @param current The current index.
	/// @return The next index in the circular buffer.
	std::size_t nextIndex(std::size_t current) const
	{
		return (current + 1) % capacity;
	}
};

#else

#include <atomic>
#include <cassert>
#include <mutex>
#include <thread>
#include <vector>
namespace isobus
{
	using Mutex = std::mutex;
	using RecursiveMutex = std::recursive_mutex;
	using Thread = std::thread;
}
/// @brief A macro to automatically lock a mutex and unlock it when the scope ends.
/// @param type The type of the mutex.
/// @param x The mutex to lock.
#define LOCK_GUARD(type, x) const std::lock_guard<type> x##Lock(x)

/// @brief A template class for a lock free queue.
/// @tparam T The item type for the queue.
template<typename T>
class LockFreeQueue
{
public:
	/// @brief Constructor for the lock free queue.
	explicit LockFreeQueue(std::size_t size) :
	  buffer(size > 0 ? size : 1), capacity(size > 0 ? size : 1)
	{
		// Validate the size of the queue. The size==0 fallback must be applied in
		// the member initializer list: the previous implementation assigned the
		// local `size` parameter in the constructor body AFTER buffer/capacity had
		// already been constructed with 0, which left capacity==0 and made
		// nextIndex() divide by zero when assertions are disabled (NDEBUG).
		assert(size > 0 && "The size of the queue must be greater than 0.");
	}

	/// @brief Push an item to the queue.
	/// @param item The item to push to the queue.
	/// @return True if the item was pushed to the queue, false if the queue is full.
	bool push(const T &item)
	{
		const auto currentWriteIndex = writeIndex.load(std::memory_order_relaxed);
		const auto nextWriteIndex = nextIndex(currentWriteIndex);

		if (nextWriteIndex == readIndex.load(std::memory_order_acquire))
		{
			// The buffer is full.
			return false;
		}

		buffer[currentWriteIndex] = item;
		writeIndex.store(nextWriteIndex, std::memory_order_release);
		return true;
	}

	/// @brief Peek at the next item in the queue.
	/// @param item The item to peek at in the queue.
	/// @return True if the item was peeked at in the queue, false if the queue is empty.
	bool peek(T &item)
	{
		const auto currentReadIndex = readIndex.load(std::memory_order_relaxed);
		if (currentReadIndex == writeIndex.load(std::memory_order_acquire))
		{
			// The buffer is empty.
			return false;
		}

		item = buffer[currentReadIndex];
		return true;
	}

	/// @brief Pop an item from the queue.
	/// @return True if the item was popped from the queue, false if the queue is empty.
	bool pop()
	{
		const auto currentReadIndex = readIndex.load(std::memory_order_relaxed);
		if (currentReadIndex == writeIndex.load(std::memory_order_acquire))
		{
			// The buffer is empty.
			return false;
		}

		readIndex.store(nextIndex(currentReadIndex), std::memory_order_release);
		return true;
	}

	/// @brief Check if the queue is full.
	/// @return True if the queue is full, false if the queue is not full.
	bool is_full() const
	{
		return nextIndex(writeIndex.load(std::memory_order_acquire)) == readIndex.load(std::memory_order_acquire);
	}

	/// @brief Clear the queue.
	void clear()
	{
		// Simply move the read index to the write index.
		readIndex.store(writeIndex.load(std::memory_order_acquire), std::memory_order_release);
	}

private:
	std::vector<T> buffer; ///< The buffer for the circular buffer.
	std::atomic<std::size_t> readIndex = { 0 }; ///< The read index for the circular buffer.
	std::atomic<std::size_t> writeIndex = { 0 }; ///< The write index for the circular buffer.
	const std::size_t capacity; ///< The capacity of the circular buffer.

	/// @brief Get the next index in the circular buffer.
	/// @param current The current index.
	/// @return The next index in the circular buffer.
	std::size_t nextIndex(std::size_t current) const
	{
		return (current + 1) % capacity;
	}
};

#endif

#include <queue>
template<typename T>
class UnsafeQueue
{
public:
	using value_type = T;

	template<typename U, typename = typename std::enable_if<std::is_convertible<U, value_type>::value>::type>
	void push(U &&item)
	{
		queue.push(std::forward<U>(item));
	}

	bool pop(value_type *item)
	{
		if (queue.empty())
		{
			return false;
		}
		*item = std::move(queue.front());
		queue.pop();
		return true;
	}

	void clear()
	{
		queue = {};
	}

private:
	std::queue<value_type> queue;
};

#if defined CAN_STACK_DISABLE_THREADS || defined ARDUINO
template<typename T>
using Queue = UnsafeQueue<T>;
#elif defined USE_CMSIS_RTOS2_THREADING

// Під CMSIS RTOS2 використовуємо CMSIS-мьютекс + LockGuard (std::mutex недоступний).
template<typename T>
class SafeQueue : private UnsafeQueue<T>
{
	using Q = UnsafeQueue<T>;

public:
	using value_type = T;

	template<typename U, typename = typename std::enable_if<std::is_convertible<U, value_type>::value>::type>
	void push(U &&item)
	{
		const isobus::LockGuard<isobus::Mutex> lock(&mtx);
		Q::push(std::forward<U>(item));
	}

	bool pop(value_type *item)
	{
		const isobus::LockGuard<isobus::Mutex> lock(&mtx);
		return Q::pop(item);
	}

	void clear()
	{
		const isobus::LockGuard<isobus::Mutex> lock(&mtx);
		Q::clear();
	}

private:
	isobus::Mutex mtx;
};
template<typename T>
using Queue = SafeQueue<T>;
#else

#include <mutex>

template<typename T>
class SafeQueue : private UnsafeQueue<T>
{
	using Q = UnsafeQueue<T>;

public:
	using value_type = T;

	template<typename U, typename = typename std::enable_if<std::is_convertible<U, value_type>::value>::type>
	void push(U &&item)
	{
		std::lock_guard<std::mutex> lock(mtx);
		Q::push(std::forward<U>(item));
	}

	bool pop(value_type *item)
	{
		std::lock_guard<std::mutex> lock(mtx);
		return Q::pop(item);
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(mtx);
		Q::clear();
	}

private:
	std::mutex mtx;
};
template<typename T>
using Queue = SafeQueue<T>;
#endif

#endif // THREAD_SYNCHRONIZATION_HPP