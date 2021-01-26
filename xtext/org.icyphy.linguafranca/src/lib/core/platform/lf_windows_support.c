/* Windows API support for the C target of Lingua Franca. */

/*************
Copyright (c) 2019, The University of California at Berkeley.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
***************/

/** Windows API support for the C target of Lingua Franca.
 *  
 *  @author{Soroush Bateni <soroush@utdallas.edu>}
 * 
 * All functions return 0 on success.
 * 
 * @see https://gist.github.com/Soroosh129/127d1893fa4c1da6d3e1db33381bb273
 */

#ifndef LF_WINDOWS_SUPPORT
#define LF_WINDOWS_SUPPORT

#include <windows.h>
#include <process.h>

/**
 * On Windows, one could use botha  mutex or
 * a critical section for the same purpose. However,
 * critical sections are lighter and limited to one process
 * and thus fit the requirements of Lingua Franca.
 */
typedef CRITICAL_SECTION __lf_mutex_t;
/**
 * For compatibility with other platform APIs, we assume
 * that mutex is analogous to critical section.
 */
typedef __lf_mutex_t __lf_critical_section_t

typedef CONDITION_VARIABLE __lf_cond_t;
typedef HANDLE __lf_thread_t;
typedef struct timespec __lf_time_spec_t;
typedef clockid_t __lf_clock_t;

#define __LF_TIMEOUT ETIMEDOUT

NtDelayExecution_t *NtDelayExecution = NULL;
NtQueryPerformanceCounter_t *NtQueryPerformanceCounter = NULL;
NtQuerySystemTime_t *NtQuerySystemTime = NULL;

/**
 * Create a new thread, starting with execution of lf_thread
 * getting passed arguments. The new handle is stored in thread.
 */
int lf_thread_create(__lf_thread_t* thread, void *(*lf_thread) (void *), void* arguments) {
    uintptr_t handle = _beginthread((windows_thread)lf_thread,0,arg);
	thread->handle = (HANDLE)handle;
	if(thread->handle == (HANDLE)-1){
		return 1;
	}else{
		return 0;
	}
}

/**
 * Make calling thread wait for termination of the thread.  The
 * exit status of the thread is stored in thread_return, if thread_return
 * is not NULL.
 */
int lf_thread_join(__lf_thread_t thread, void** thread_return) {    
	DWORD retvalue = WaitForSingleObject(thread.handle,INFINITE);
	if(retvalue == WAIT_OBJECT_0){
		return 0;
	}else{
		return EINVAL;
	}
}

/**
 * Initialize a critical section.
 */
int lf_mutex_init(__lf_critical_section_t* critical_section) {
    // Set up a recursive mutex
	InitializeCriticalSection((CRITICAL_SECTION*)critical_section);
	if(critical_section != NULL){
		return 0;
	}else{
		return 1;
	}
}

/** 
 * Lock a critical section.
 * 
 * From https://docs.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-entercriticalsection:
 *    "This function can raise EXCEPTION_POSSIBLE_DEADLOCK if a wait operation on the critical section times out.
 *     The timeout interval is specified by the following registry value: 
 *     HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\CriticalSectionTimeout.
 *     Do not handle a possible deadlock exception; instead, debug the application."
 */
int lf_mutex_lock(__lf_critical_section_t* critical_section) {
	EnterCriticalSection((CRITICAL_SECTION*)critical_section);
    return 0;
}

/** 
 * Leave a critical_section.
 */
int lf_mutex_unlock(__lf_critical_section_t* critical_section) {
    LeaveCriticalSection((CRITICAL_SECTION*)critical_section);
    return 0;
}

/* Initialize a conditional variable. */
int lf_cond_init(__lf_cond_t* cond) {
    InitializeConditionVariable((CONDITION_VARIABLE*)cond);
    return 0;
}

/* Wake up all threads waiting for condition variable cond.  */
int lf_cond_broadcast(__lf_cond_t* cond) {
    WakeAllConditionVariable((CONDITION_VARIABLE*)cond);
    return 0;
}

/* Wake up one thread waiting for condition variable cond.  */
int lf_cond_signal(__lf_cond_t* cond) {
    WakeConditionVariable((CONDITION_VARIABLE*)cond);
    return 0;
}

/* Wait for condition variable COND to be signaled or broadcast.
   MUTEX is assumed to be locked before. */
int lf_cond_wait(__lf_cond_t* cond, __lf_critical_section_t* critical_section) {
    return (int)SleepConditionVariableCS((CONDITION_VARIABLE*)cond, (CRITICAL_SECTION*)critical_section, INFINITE);
}

/** 
 * Block current thread on the condition variable until condition variable
 * pointed by __COND is signaled or time pointed by __TIME_POINT is
 * reached.
 * 
 * @return 0 on success and LF_TIMEOUT on timeout.
 */
int lf_cond_timedwait(__lf_cond_t* cond, __lf_critical_section_t* critical_section, instant_t absolute_time_ns) {
    // Convert the absolute time to a relative time
    DWORD relative_time_ms = (absolute_time_ns - get_start_time())/1000000LL;

    return (int)SleepConditionVariableCS((CONDITION_VARIABLE*)cond, (CRITICAL_SECTION*)critical_section, relative_time_ms);
}

/**
 * Fetch the value of clk_id and store it in tp.
 */
int lf_clock_gettime(__lf_clock_t clk_id, __lf_time_spec_t* tp) {
    int result = -1;
    int days_from_1601_to_1970 = 134774 /* there were no leap seconds during this time, so life is easy */;
    long long timestamp, counts, counts_per_sec;
    switch (clk_id) {
    case CLOCK_REALTIME:
        NtQuerySystemTime((PLARGE_INTEGER)&timestamp);
        timestamp -= days_from_1601_to_1970 * 24LL * 60 * 60 * 1000 * 1000 * 10;
        tp->tv_sec = (time_t)(timestamp / (BILLION / 100));
        tp->tv_nsec = (long)((timestamp % (BILLION / 100)) * 100);
        result = 0;
        break;
    case CLOCK_MONOTONIC:
        if ((*NtQueryPerformanceCounter)((PLARGE_INTEGER)&counts, (PLARGE_INTEGER)&counts_per_sec) == 0) {
            tp->tv_sec = counts / counts_per_sec;
            tp->tv_nsec = (long)((counts % counts_per_sec) * BILLION / counts_per_sec);
            result = 0;
        } else {
            errno = EINVAL;
            result = -1;
        }
        break;
    default:
        errno = EINVAL;
        result = -1;
        break;
    }
    return result;
}

/**
 * Pause execution for a number of nanoseconds.
 */
int lf_nanosleep(__lf_clock_t clk_id, const __lf_time_spec_t* requested_time, __lf_time_spec_t* remaining) {
    unsigned char alertable = remaining ? 1 : 0;
    long long duration = -(requested_time->tv_sec * (BILLION / 100) + requested_time->tv_nsec / 100);
    NTSTATUS status = (*NtDelayExecution)(alertable, (PLARGE_INTEGER)&duration);
    int result = status == 0 ? 0 : -1;
    if (alertable) {
        if (status < 0) {
            errno = EINVAL;
        } else if (status > 0 && lf_clock_gettime(clk_id, remaining) == 0) {
            errno = EINTR;
        }
    }
    return result;
}

#endif // LF_WINDOWS_SUPPORT