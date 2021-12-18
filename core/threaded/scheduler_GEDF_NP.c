/* Global Earliest Deadline First (GEDF) non-preemptive scheduler for the
threaded runtime of the C target of Lingua Franca. */

/*************
Copyright (c) 2021, The University of Texas at Dallas.
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

/** 
 * Global Earliest Deadline First (GEDF) non-preemptive scheduler for the
 * threaded runtime of the C target of Lingua Franca.
 *  
 * @author{Soroush Bateni <soroush@utdallas.edu>}
 * @author{Edward A. Lee <eal@berkeley.edu>}
 * @author{Marten Lohstroh <marten@berkeley.edu>}
 */

#ifndef NUMBER_OF_WORKERS
#define NUMBER_OF_WORKERS 1
#endif // NUMBER_OF_WORKERS

#ifndef MAX_REACTION_LEVEL
#define MAX_REACTION_LEVEL INITIAL_REACT_QUEUE_SIZE
#endif

#include "scheduler.h"
#include "../platform.h"
#include "../utils/semaphore.c"
#include "../utils/pqueue_support.h"
#include <assert.h>


/////////////////// External Variables /////////////////////////
extern lf_mutex_t mutex;
extern tag_t current_tag;
extern tag_t stop_tag;

/////////////////// External Functions /////////////////////////
/**
 * Placeholder for function that will advance tag and initially fill the
 * reaction queue.
 * 
 * This does not acquire the mutex lock. It assumes the lock is already held.
 */
void _lf_next_locked();

/** 
 * Placeholder for code-generated function that will, in a federated
 * execution, be used to coordinate the advancement of tag. It will notify
 * the runtime infrastructure (RTI) that all reactions at the specified
 * logical tag have completed. This function should be called only while
 * holding the mutex lock.
 * @param tag_to_send The tag to send.
 */
void logical_tag_complete(tag_t tag_to_send);

/////////////////// Scheduler Variables and Structs /////////////////////////
/**
 * @brief Atomically keep track of how many worker threads are idle.
 *
 * Initially assumed that there are 0 idle threads.
 */
semaphore_t* _lf_sched_semaphore; 

/**
 * @brief Indicate whether the program should stop
 * 
 */
volatile bool _lf_sched_should_stop = false;

/**
 * @brief FIXME
 * 
 */
pqueue_t* vector_of_reaction_qs[MAX_REACTION_LEVEL + 1];

/**
 * @brief Queue of currently executing reactions.
 * 
 * Sorted by index (precedence sort)
 */
pqueue_t* executing_q;

/**
 * @brief Number of workers that this scheduler is managing.
 * 
 */
size_t _lf_sched_number_of_workers = 1;

/**
 * @brief Number of workers that are idle.
 * 
 * Adding to/subtracting from this variable must be done atomically.
 * 
 */
volatile size_t _lf_sched_number_of_idle_workers = 0;

/**
 * @brief Indicator that execution of at least one tag has completed.
 */
bool _lf_logical_tag_completed = false;

/**
 * @brief Mutex that must be acquired by workers before accessing the executing_q.
 * 
 */
lf_mutex_t _lf_sched_executing_q_mutex;

/**
 * @brief The current level of reactions (either executing or to execute).
 * 
 */
volatile size_t _lf_sched_next_reaction_level = 0;

/////////////////// Scheduler Private API /////////////////////////
/**
 * @brief Insert 'reaction' into vector_of_reaction_qs at the appropriate level.
 * 
 * If there is not pqueue at the level of 'reaction', this function will
 * initialize one.
 * 
 * @param reaction The reaction to insert.
 */
static inline void _lf_sched_insert_reaction(reaction_t* reaction) {
    size_t reaction_level = LEVEL(reaction->index);
#ifdef FEDERATED
    // Inserting reactions at the same level as executing reactions can only
    // happen in federated execution where network input control reactions can
    // block until the network receiver reaction (which has the same level as
    // the network input control reaction) is triggered.
    bool locked = false;
    if (reaction_level == (_lf_sched_next_reaction_level-1)) {
        // If inserting a reaction at a level that matches the current executing
        // level, lock the executing queue mutex because worker threads are also
        // accessing this queue.
        DEBUG_PRINT("Scheduler: Locking the executing queue mutex.");
        lf_mutex_lock(&_lf_sched_executing_q_mutex);
        locked = true;
    }
#endif
    pqueue_insert(vector_of_reaction_qs[reaction_level], (void*)reaction);
#ifdef FEDERATED
    if (locked) {
        lf_mutex_unlock(&_lf_sched_executing_q_mutex);
    }
#endif
}

/**
 * @brief Distribute any reaction that is ready to execute to idle worker thread(s).
 * 
 * This assumes that the caller is holding 'mutex' and is not holding any thread mutexes.
 * 
 * @return Number of reactions that were successfully distributed to worker threads.
 */ 
int _lf_sched_distribute_ready_reactions_locked() {
    for (;_lf_sched_next_reaction_level <= MAX_REACTION_LEVEL; _lf_sched_next_reaction_level++) {
        executing_q = vector_of_reaction_qs[_lf_sched_next_reaction_level];
        size_t reactions_to_execute = pqueue_size(executing_q);
        if (reactions_to_execute) {
            _lf_sched_next_reaction_level++;
            return reactions_to_execute;
        }
    }
    
    return 0;
}

/**
 * Return true if the worker should stop now; false otherwise.
 * This function assumes the caller holds the mutex lock.
 */
bool _lf_sched_should_stop_locked() {
    // If this is not the very first step, notify that the previous step is complete
    // and check against the stop tag to see whether this is the last step.
    if (_lf_logical_tag_completed) {
        logical_tag_complete(current_tag);
        // If we are at the stop tag, do not call _lf_next_locked()
        // to prevent advancing the logical time.
        if (compare_tags(current_tag, stop_tag) >= 0) {
            return true;
        }
    }
    return false;
}

/**
 * Advance tag. This will also pop events for the newly acquired tag and put
 * the triggered reactions on the 'vector_of_reaction_qs'.
 * 
 * This function assumes the caller holds the 'mutex' lock.
 * 
 * @return should_exit True if the worker thread should exit. False otherwise.
 */
bool _lf_sched_advance_tag_locked() {

    if (_lf_sched_should_stop_locked()) {
        return true;
    }

    _lf_logical_tag_completed = true;

    // Advance time.
    // _lf_next_locked() may block waiting for real time to pass or events to appear.
    // to appear on the event queue. Note that we already
    // hold the mutex lock.
    // tracepoint_worker_advancing_time_starts(worker_number); 
    // FIXME: Tracing should be updated to support scheduler events
    _lf_next_locked();

    DEBUG_PRINT("Scheduler: Done waiting for _lf_next_locked().");
    return false;
}

/**
 * @brief If there is work to be done, notify workers individually.
 * 
 * This assumes that the caller is not holding any thread mutexes.
 */
void _lf_sched_notify_workers() {
    size_t workers_to_be_awaken = MIN(_lf_sched_number_of_idle_workers, pqueue_size(executing_q));
    DEBUG_PRINT("Scheduler: Notifying %d workers.", workers_to_be_awaken);
    _lf_sched_number_of_idle_workers -= workers_to_be_awaken;
    DEBUG_PRINT("Scheduler: New number of idle workers: %u.", _lf_sched_number_of_idle_workers);
    if (workers_to_be_awaken > 1) {
        // Notify all the workers except the worker thread that has called this function. 
        lf_semaphore_release(_lf_sched_semaphore, (workers_to_be_awaken-1));
    }
}

/**
 * @brief Signal all worker threads that it is time to stop.
 * 
 */
void _lf_sched_signal_stop() {
    _lf_sched_should_stop = true;
    lf_semaphore_release(_lf_sched_semaphore, (_lf_sched_number_of_workers - 1));
}

/**
 * @brief Advance tag or distribute reactions to worker threads.
 *
 * Advance tag if there are no reactions on the reaction queue. If
 * there are such reactions, distribute them to worker threads.
 * 
 * This function assumes the caller does not hold the 'mutex' lock.
 */
void _lf_sched_try_advance_tag_and_distribute() {
    // Loop until it's time to stop or work has been distributed
    while (true) {
        if (_lf_sched_next_reaction_level == (MAX_REACTION_LEVEL + 1)) {
            _lf_sched_next_reaction_level = 0;
            lf_mutex_lock(&mutex);
            // Nothing more happening at this tag.
            DEBUG_PRINT("Scheduler: Advancing tag.");
            // This worker thread will take charge of advancing tag.
            if (_lf_sched_advance_tag_locked()) {
                DEBUG_PRINT("Scheduler: Reached stop tag.");
                _lf_sched_signal_stop();
                lf_mutex_unlock(&mutex);
                break;
            }
            lf_mutex_unlock(&mutex);
        }

        if (_lf_sched_distribute_ready_reactions_locked() > 0) {
            _lf_sched_notify_workers();
            break;
        }
    }
}

/**
 * @brief Wait until the scheduler assigns work.
 *
 * If the calling worker thread is the last to become idle, it will call on the
 * scheduler to distribute work. Otherwise, it will wait on '_lf_sched_semaphore'.
 *
 * @param worker_number The worker number of the worker thread asking for work
 * to be assigned to it.
 */
void _lf_sched_wait_for_work(size_t worker_number) {
    // Increment the number of idle workers by 1 and check if this is the last
    // worker thread to become idle.
    if (lf_atomic_fetch_add(&_lf_sched_number_of_idle_workers, 1) == (_lf_sched_number_of_workers - 1)) {
        // Last thread to go idle
        DEBUG_PRINT("Scheduler: Worker %d is the last idle thread.", worker_number);
        // Call on the scheduler to distribute work or advance tag.
        _lf_sched_try_advance_tag_and_distribute();
    } else {
        // Not the last thread to become idle.
        // Wait for work to be released.
        DEBUG_PRINT("Scheduler: Worker %d is trying to acquire the scheduling semaphore.", worker_number);
        lf_semaphore_acquire(_lf_sched_semaphore);
        DEBUG_PRINT("Scheduler: Worker %d acquired the scheduling semaphore.", worker_number);
    }
}

///////////////////// Scheduler Init and Destroy API /////////////////////////
/**
 * @brief Initialize the scheduler.
 * 
 * This has to be called before other functions of the scheduler can be used.
 * 
 * @param number_of_workers Indicate how many workers this scheduler will be managing.
 */
void lf_sched_init(size_t number_of_workers) {
    DEBUG_PRINT("Scheduler: Initializing with %d workers", number_of_workers);
    
    _lf_sched_semaphore = lf_semaphore_new(0);
    _lf_sched_number_of_workers = number_of_workers;

    for (size_t i = 0; i <= MAX_REACTION_LEVEL; i++) {
        vector_of_reaction_qs[i] = pqueue_init(INITIAL_REACT_QUEUE_SIZE, in_reverse_order, get_reaction_index,
                get_reaction_position, set_reaction_position, reaction_matches, print_reaction);
    }

    // Create a queue on which to put reactions that are currently executing.
    executing_q = vector_of_reaction_qs[0];
    
    _lf_sched_should_stop = false;

    lf_mutex_init(&_lf_sched_executing_q_mutex);
}

/**
 * @brief Free the memory used by the scheduler.
 * 
 * This must be called when the scheduler is no longer needed.
 */
void lf_sched_free() {
    // for (size_t j = 0; j <= MAX_REACTION_LEVEL; j++) {
    //     pqueue_free(vector_of_reaction_qs[j]); FIXME: This is causing weird
    //     memory errors.
    // }
    pqueue_free(executing_q);
    if (lf_semaphore_destroy(_lf_sched_semaphore) != 0) {
        error_print_and_exit("Scheduler: Could not destroy my semaphore.");
    }
}

///////////////////// Scheduler Worker API (public) /////////////////////////
/**
 * @brief Ask the scheduler for one more reaction.
 * 
 * If there is a ready reaction for worker thread 'worker_number', then a
 * reaction will be returned. If not, this function will block and ask the
 * scheduler for more work. Once work is delivered, it will return a ready
 * reaction. When it's time for the worker thread to stop and exit, it will
 * return NULL.
 * 
 * @param worker_number 
 * @return reaction_t* A reaction for the worker to execute. NULL if the calling
 * worker thread should exit.
 */
reaction_t* lf_sched_get_ready_reaction(int worker_number) {
    // Iterate until the stop_tag is reached or reaction queue is empty
    while (!_lf_sched_should_stop) {
        lf_mutex_lock(&_lf_sched_executing_q_mutex);
        reaction_t* reaction_to_return = (reaction_t*)pqueue_pop(executing_q);
        lf_mutex_unlock(&_lf_sched_executing_q_mutex);
        
        if (reaction_to_return != NULL) {
            // Got a reaction
            return reaction_to_return;
        }

        DEBUG_PRINT("Worker %d is out of ready reactions.", worker_number);

        // Ask the scheduler for more work and wait
        _lf_sched_wait_for_work(worker_number);
    }

    // It's time for the worker thread to stop and exit.
    return NULL;
}

/**
 * @brief Inform the scheduler that worker thread 'worker_number' is done
 * executing the 'done_reaction'.
 * 
 * @param worker_number The worker number for the worker thread that has
 * finished executing 'done_reaction'.
 * @param done_reaction The reaction is that is done.
 */
void lf_sched_done_with_reaction(size_t worker_number, reaction_t* done_reaction) {
    if (!lf_bool_compare_and_swap(&done_reaction->status, queued, inactive)) {
        error_print_and_exit("Unexpected reaction status: %d. Expected %d.", 
            done_reaction->status,
            queued);
    }
}

/**
 * @brief Inform the scheduler that worker thread 'worker_number' would like to
 * trigger 'reaction' at the current tag.
 * 
 * This triggering happens lazily (at a later point when the scheduler deems
 * appropriate), unless worker_number is set to -1. In that case, the triggering
 * of 'reaction' is done immediately.
 * 
 * The scheduler will ensure that the same reaction is not triggered twice in
 * the same tag.
 * 
 * @param reaction The reaction to trigger at the current tag.
 * @param worker_number The ID of the worker that is making this call. 0 should be
 *  used if there is only one worker (e.g., when the program is using the
 *  unthreaded C runtime). -1 is used for an anonymous call in a context where a
 *  worker number does not make sense (e.g., the caller is not a worker thread).
 */
void lf_sched_trigger_reaction(reaction_t* reaction, int worker_number) {
    // The scheduler should handle this immediately
    // Protect against putting a reaction twice on the reaction queue by
    // checking its status.
    if (reaction != NULL && lf_bool_compare_and_swap(&reaction->status, inactive, queued)) {
        DEBUG_PRINT("Scheduler: Enqueing reaction %s, which has level %lld.",
                    reaction->name, LEVEL(reaction->index));
        // Immediately put 'reaction' on the reaction queue.
        _lf_sched_insert_reaction(reaction);
    }
    return;
}