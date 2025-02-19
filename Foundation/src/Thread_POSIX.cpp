//
// Thread_POSIX.cpp
//
// $Id: //poco/1.4/Foundation/src/Thread_POSIX.cpp#5 $
//
// Library: Foundation
// Package: Threading
// Module:  Thread
//
// Copyright (c) 2004-2007, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// SPDX-License-Identifier:	BSL-1.0
//


#include "Poco/Thread_POSIX.h"
#include "Poco/Thread.h"
#include "Poco/Exception.h"
#include "Poco/ErrorHandler.h"
#include "Poco/Timespan.h"
#include "Poco/Timestamp.h"
#include <signal.h>
#if defined(__sun) && defined(__SVR4)
#	if !defined(__EXTENSIONS__)
#		define __EXTENSIONS__
#	endif
#endif
#if POCO_OS == POCO_OS_LINUX || POCO_OS == POCO_OS_MAC_OS_X || POCO_OS == POCO_OS_QNX
#	include <time.h>
#	include <unistd.h>
#endif
#if POCO_OS == POCO_OS_MAC_OS_X
#   include <mach/mach.h>
#   include <mach/task.h>
#   include <mach/thread_policy.h>
#endif
#if POCO_OS == POCO_OS_LINUX
#include <sys/syscall.h>
#endif
#include <cstring>


//
// Block SIGPIPE in main thread.
//
#if defined(POCO_OS_FAMILY_UNIX) && !defined(POCO_VXWORKS)
namespace {
class SignalBlocker
{
public:
	SignalBlocker()
	{
		sigset_t sset;
		sigemptyset(&sset);
		sigaddset(&sset, SIGPIPE);
		pthread_sigmask(SIG_BLOCK, &sset, 0);
	}
	~SignalBlocker()
	{
	}
};


static SignalBlocker signalBlocker;
}
#endif


#if defined(POCO_POSIX_DEBUGGER_THREAD_NAMES)


namespace {
void setThreadName(pthread_t thread, const std::string& threadName)
{
#if (POCO_OS == POCO_OS_MAC_OS_X)
	pthread_setname_np(threadName.c_str()); // __OSX_AVAILABLE_STARTING(__MAC_10_6, __IPHONE_3_2)
#else
	if (pthread_setname_np(thread, threadName.c_str()) != 0 && errno == ERANGE && threadName.size() > 15)
	{
		std::string truncName(threadName, 0, 7);
		truncName.append("~");
		truncName.append(threadName, threadName.size() - 7, 7);
		pthread_setname_np(thread, truncName.c_str());
	}
#endif
}
}


#endif // POCO_POSIX_DEBUGGER_THREAD_NAMES


namespace Poco {


ThreadImpl::CurrentThreadHolder ThreadImpl::_currentThreadHolder;


ThreadImpl::ThreadImpl():
	_pData(new ThreadData)
{
}


ThreadImpl::~ThreadImpl()
{
	if (_pData->started && !_pData->joined)
	{
		pthread_detach(_pData->thread);
	}
}


void ThreadImpl::setPriorityImpl(int prio)
{
	if (prio != _pData->prio)
	{
		_pData->prio = prio;
		_pData->policy = SCHED_OTHER;
		if (isRunningImpl())
		{
			struct sched_param par;
			struct MyStruct
			{

			};
			par.sched_priority = mapPrio(_pData->prio, SCHED_OTHER);
			if (pthread_setschedparam(_pData->thread, SCHED_OTHER, &par))
				throw SystemException("cannot set thread priority");
		}
	}
}


void ThreadImpl::setOSPriorityImpl(int prio, int policy)
{
	if (prio != _pData->osPrio || policy != _pData->policy)
	{
		if (_pData->pRunnableTarget)
		{
			struct sched_param par;
			par.sched_priority = prio;
			if (pthread_setschedparam(_pData->thread, policy, &par))
				throw SystemException("cannot set thread priority");
		}
		_pData->prio   = reverseMapPrio(prio, policy);
		_pData->osPrio = prio;
		_pData->policy = policy;
	}
}


int ThreadImpl::getMinOSPriorityImpl(int policy)
{
#if defined(POCO_THREAD_PRIORITY_MIN)
	return POCO_THREAD_PRIORITY_MIN;
#elif defined(__VMS) || defined(__digital__)
	return PRI_OTHER_MIN;
#else
	return sched_get_priority_min(policy);
#endif
}


int ThreadImpl::getMaxOSPriorityImpl(int policy)
{
#if defined(POCO_THREAD_PRIORITY_MAX)
	return POCO_THREAD_PRIORITY_MAX;
#elif defined(__VMS) || defined(__digital__)
	return PRI_OTHER_MAX;
#else
	return sched_get_priority_max(policy);
#endif
}


void ThreadImpl::setStackSizeImpl(int size)
{
#ifndef PTHREAD_STACK_MIN
	_pData->stackSize = 0;
#else
	if (size != 0)
	{
#if defined(POCO_OS_FAMILY_BSD)
		// we must round up to a multiple of the memory page size
		const int STACK_PAGE_SIZE = 4096;
		size = ((size + STACK_PAGE_SIZE - 1) / STACK_PAGE_SIZE) * STACK_PAGE_SIZE;
#endif
#if !defined(POCO_ANDROID)
		if (size < PTHREAD_STACK_MIN)
			size = PTHREAD_STACK_MIN;
#endif
	}
	_pData->stackSize = size;
#endif
}


void ThreadImpl::setAffinityImpl(int cpu)
{
#if defined (POCO_OS_FAMILY_UNIX) && POCO_OS != POCO_OS_MAC_OS_X
#ifdef HAVE_PTHREAD_SETAFFINITY_NP
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
#ifdef HAVE_THREE_PARAM_SCHED_SETAFFINITY
	if (pthread_setaffinity_np(_pData->thread, sizeof(cpuset), &cpuset) != 0)
		throw SystemException("Failed to set affinity");
#else
	if (pthread_setaffinity_np(_pData->thread, &cpuset) != 0)
		throw SystemException("Failed to set affinity");
#endif
#endif
#endif // defined unix & !defined mac os x

#if POCO_OS == POCO_OS_MAC_OS_X
	kern_return_t ret;
	thread_affinity_policy policy;
	policy.affinity_tag = cpu;

	ret = thread_policy_set(pthread_mach_thread_np(_pData->thread),
				THREAD_AFFINITY_POLICY,
				(thread_policy_t) &policy,
				THREAD_AFFINITY_POLICY_COUNT);
	if (ret != KERN_SUCCESS)
	{
		throw SystemException("Failed to set affinity");
	}
#endif
	yieldImpl();
}


int ThreadImpl::getAffinityImpl() const
{
	int cpuSet = -1;
#if defined (POCO_OS_FAMILY_UNIX) && POCO_OS != POCO_OS_MAC_OS_X
#ifdef HAVE_PTHREAD_SETAFFINITY_NP
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
#ifdef HAVE_THREE_PARAM_SCHED_SETAFFINITY
	if (pthread_getaffinity_np(_pData->thread, sizeof(cpuset), &cpuset) != 0)
		throw SystemException("Failed to get affinity", errno);
#else
	if (pthread_getaffinity_np(_pData->thread, &cpuset) != 0)
		throw SystemException("Failed to get affinity", errno);
#endif
	int cpuCount = Environment::processorCount();
	for (int i = 0; i < cpuCount; i++)
	{
		if (CPU_ISSET(i, &cpuset))
		{
			cpuSet = i;
			break;
		}
	}
#endif
#endif // defined unix & !defined mac os x

#if POCO_OS == POCO_OS_MAC_OS_X
	kern_return_t ret;
	thread_affinity_policy policy;
	mach_msg_type_number_t count = THREAD_AFFINITY_POLICY_COUNT;
	boolean_t get_default = false;
	ret = thread_policy_get(pthread_mach_thread_np(_pData->thread),
				THREAD_AFFINITY_POLICY,
				(thread_policy_t)&policy,
				&count,
				&get_default);
	if (ret != KERN_SUCCESS)
	{
		throw SystemException("Failed to get affinity", errno);
	}
	cpuSet = policy.affinity_tag;
	int cpuCount = Environment::processorCount();
	if (cpuSet >= cpuCount)
		cpuSet = -1;

#endif
	return cpuSet;
}


void ThreadImpl::startImpl(SharedPtr<Runnable> pTarget)
{
	if (_pData->pRunnableTarget)
		throw SystemException("thread already running");

	pthread_attr_t attributes;
	pthread_attr_init(&attributes);

	if (_pData->stackSize != 0)
	{
		if (0 != pthread_attr_setstacksize(&attributes, _pData->stackSize))
		{
			pthread_attr_destroy(&attributes);
			throw SystemException("cannot set thread stack size");
		}
	}

	_pData->pRunnableTarget = pTarget;
	if (pthread_create(&_pData->thread, &attributes, runnableEntry, this))
	{
		_pData->pRunnableTarget = 0;
		pthread_attr_destroy(&attributes);
		throw SystemException("cannot start thread");
	}
#if POCO_OS == POCO_OS_LINUX
	// On Linux the TID is acquired from the running thread using syscall
	_pData->tid = 0;
#elif POCO_OS == POCO_OS_MAC_OS_X
	_pData->tid = static_cast<TIDImpl>( pthread_mach_thread_np(_pData->thread) );
#else
	_pData->tid = _pData->thread;
#endif

	_pData->started = true;
	pthread_attr_destroy(&attributes);

	if (_pData->policy == SCHED_OTHER)
	{
		if (_pData->prio != PRIO_NORMAL_IMPL)
		{
			struct sched_param par;
			par.sched_priority = mapPrio(_pData->prio, SCHED_OTHER);
			if (pthread_setschedparam(_pData->thread, SCHED_OTHER, &par))
				throw SystemException("cannot set thread priority");
		}
	}
	else
	{
		struct sched_param par;
		par.sched_priority = _pData->osPrio;
		if (pthread_setschedparam(_pData->thread, _pData->policy, &par))
			throw SystemException("cannot set thread priority");
	}
}


void ThreadImpl::joinImpl()
{
	if (!_pData->started) return;
	_pData->done.wait();
	void* result;
	if (pthread_join(_pData->thread, &result))
		throw SystemException("cannot join thread");
	_pData->joined = true;
}


bool ThreadImpl::joinImpl(long milliseconds)
{
	if (_pData->started && _pData->done.tryWait(milliseconds))
	{
		void* result;
		if (pthread_join(_pData->thread, &result))
			throw SystemException("cannot join thread");
		_pData->joined = true;
		return true;
	}
	else if (_pData->started) return false;
	else return true;
}


ThreadImpl* ThreadImpl::currentImpl()
{
	return _currentThreadHolder.get();
}


ThreadImpl::TIDImpl ThreadImpl::currentTidImpl()
{
#if POCO_OS == POCO_OS_LINUX
#ifndef SYS_gettid
#define SYS_gettid __NR_gettid
#endif
	return static_cast<TIDImpl>( syscall (SYS_gettid) );
#elif POCO_OS == POCO_OS_MAC_OS_X
	return static_cast<TIDImpl>( pthread_mach_thread_np(pthread_self()) );
#else
	return pthread_self();
#endif
}


void ThreadImpl::sleepImpl(long milliseconds)
{
#if defined(__VMS) || defined(__digital__)
	// This is specific to DECThreads
	struct timespec interval;
	interval.tv_sec  = milliseconds / 1000;
	interval.tv_nsec = (milliseconds % 1000) * 1000000;
	pthread_delay_np(&interval);
#elif POCO_OS == POCO_OS_LINUX || POCO_OS == POCO_OS_MAC_OS_X || POCO_OS == POCO_OS_QNX || POCO_OS == POCO_OS_VXWORKS
	Poco::Timespan remainingTime(1000 * Poco::Timespan::TimeDiff(milliseconds));
	int rc;
	do
	{
		struct timespec ts;
		ts.tv_sec  = (long) remainingTime.totalSeconds();
		ts.tv_nsec = (long) remainingTime.useconds() * 1000;
		Poco::Timestamp start;
		rc = ::nanosleep(&ts, 0);
		if (rc < 0 && errno == EINTR)
		{
			Poco::Timestamp end;
			Poco::Timespan waited = start.elapsed();
			if (waited < remainingTime)
				remainingTime -= waited;
			else
				remainingTime = 0;
		}
	}
	while (remainingTime > 0 && rc < 0 && errno == EINTR);
	if (rc < 0 && remainingTime > 0) throw Poco::SystemException("Thread::sleep(): nanosleep() failed");
#else
	Poco::Timespan remainingTime(1000 * Poco::Timespan::TimeDiff(milliseconds));
	int rc;
	do
	{
		struct timeval tv;
		tv.tv_sec  = (long) remainingTime.totalSeconds();
		tv.tv_usec = (long) remainingTime.useconds();
		Poco::Timestamp start;
		rc = ::select(0, NULL, NULL, NULL, &tv);
		if (rc < 0 && errno == EINTR)
		{
			Poco::Timestamp end;
			Poco::Timespan waited = start.elapsed();
			if (waited < remainingTime)
				remainingTime -= waited;
			else
				remainingTime = 0;
		}
	}
	while (remainingTime > 0 && rc < 0 && errno == EINTR);
	if (rc < 0 && remainingTime > 0) throw Poco::SystemException("Thread::sleep(): select() failed");
#endif
}


void* ThreadImpl::runnableEntry(void* pThread)
{
	_currentThreadHolder.set(reinterpret_cast<ThreadImpl*>(pThread));

#if defined(POCO_OS_FAMILY_UNIX)
	sigset_t sset;
	sigemptyset(&sset);
	sigaddset(&sset, SIGQUIT);
	sigaddset(&sset, SIGTERM);
	sigaddset(&sset, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &sset, 0);
#endif

	ThreadImpl* pThreadImpl = reinterpret_cast<ThreadImpl*>(pThread);
#if defined(POCO_POSIX_DEBUGGER_THREAD_NAMES)
	setThreadName(pThreadImpl->_pData->thread, reinterpret_cast<Thread*>(pThread)->getName());
#endif
#if POCO_OS == POCO_OS_LINUX
	pThreadImpl->_pData->tid = static_cast<TIDImpl>( syscall (SYS_gettid) );
#endif

	AutoPtr<ThreadData> pData = pThreadImpl->_pData;

	try
	{
		pData->pRunnableTarget->run();
	}
	catch (Exception& exc)
	{
		ErrorHandler::handle(exc);
	}
	catch (std::exception& exc)
	{
		ErrorHandler::handle(exc);
	}
	catch (...)
	{
		ErrorHandler::handle();
	}

	pData->pRunnableTarget = 0;
	pData->done.set();
	return 0;
}


int ThreadImpl::mapPrio(int prio, int policy)
{
	int pmin = getMinOSPriorityImpl(policy);
	int pmax = getMaxOSPriorityImpl(policy);

	switch (prio)
	{
	case PRIO_LOWEST_IMPL:
		return pmin;
	case PRIO_LOW_IMPL:
		return pmin + (pmax - pmin) / 4;
	case PRIO_NORMAL_IMPL:
		return pmin + (pmax - pmin) / 2;
	case PRIO_HIGH_IMPL:
		return pmin + 3 * (pmax - pmin) / 4;
	case PRIO_HIGHEST_IMPL:
		return pmax;
	default:
		poco_bugcheck_msg("invalid thread priority");
	}
	return -1; // just to satisfy compiler - we'll never get here anyway
}


int ThreadImpl::reverseMapPrio(int prio, int policy)
{
	if (policy == SCHED_OTHER)
	{
		int pmin = getMinOSPriorityImpl(policy);
		int pmax = getMaxOSPriorityImpl(policy);
		int normal = pmin + (pmax - pmin) / 2;
		if (prio == pmax)
			return PRIO_HIGHEST_IMPL;
		if (prio > normal)
			return PRIO_HIGH_IMPL;
		else if (prio == normal)
			return PRIO_NORMAL_IMPL;
		else if (prio > pmin)
			return PRIO_LOW_IMPL;
		else
			return PRIO_LOWEST_IMPL;
	}
	else return PRIO_HIGHEST_IMPL;
}


} // namespace Poco
