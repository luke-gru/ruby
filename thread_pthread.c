/* -*-c-*- */
/**********************************************************************

  thread_pthread.c -

  $Author$

  Copyright (C) 2004-2007 Koichi Sasada

**********************************************************************/

#ifdef THREAD_SYSTEM_DEPENDENT_IMPLEMENTATION

#include "internal/gc.h"
#include "internal/sanitizers.h"
#include "rjit.h"

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#ifdef HAVE_THR_STKSEGMENT
#include <thread.h>
#endif
#if defined(HAVE_FCNTL_H)
#include <fcntl.h>
#elif defined(HAVE_SYS_FCNTL_H)
#include <sys/fcntl.h>
#endif
#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif
#if defined(HAVE_SYS_TIME_H)
#include <sys/time.h>
#endif
#if defined(__HAIKU__)
#include <kernel/OS.h>
#endif
#ifdef __linux__
#include <sys/syscall.h> /* for SYS_gettid */
#endif
#include <time.h>
#include <signal.h>

#if defined __APPLE__
# include <AvailabilityMacros.h>
#endif

#if defined(HAVE_SYS_EVENTFD_H) && defined(HAVE_EVENTFD)
#  define USE_EVENTFD (1)
#  include <sys/eventfd.h>
#else
#  define USE_EVENTFD (0)
#endif

#if defined(HAVE_PTHREAD_CONDATTR_SETCLOCK) && \
    defined(CLOCK_REALTIME) && defined(CLOCK_MONOTONIC) && \
    defined(HAVE_CLOCK_GETTIME)
static pthread_condattr_t condattr_mono;
static pthread_condattr_t *condattr_monotonic = &condattr_mono;
#else
static const void *const condattr_monotonic = NULL;
#endif

#include COROUTINE_H

#ifndef HAVE_SYS_EVENT_H
#define HAVE_SYS_EVENT_H 0
#endif

#ifndef HAVE_SYS_EPOLL_H
#define HAVE_SYS_EPOLL_H 0
#else
// force setting for debug
// #undef HAVE_SYS_EPOLL_H
// #define HAVE_SYS_EPOLL_H 0
#endif

#ifndef USE_MN_THREADS
  #if defined(__EMSCRIPTEN__) || defined(COROUTINE_PTHREAD_CONTEXT)
    // on __EMSCRIPTEN__ provides epoll* declarations, but no implementations.
    // on COROUTINE_PTHREAD_CONTEXT, it doesn't worth to use it.
    #define USE_MN_THREADS 0
  #elif HAVE_SYS_EPOLL_H
    #include <sys/epoll.h>
    #define USE_MN_THREADS 1
  #elif HAVE_SYS_EVENT_H
    #include <sys/event.h>
    #define USE_MN_THREADS 1
  #else
    #define USE_MN_THREADS 0
  #endif
#endif

// native thread wrappers

extern rb_atomic_t ruby_nt_serial;
int timer_thread_tid = 0;

#if defined(RB_THREAD_T_HAS_NATIVE_ID) || defined(__APPLE__)
static int
get_native_thread_id(void)
{
#ifdef __linux__
    return (int)syscall(SYS_gettid);
#elif defined(__FreeBSD__)
    return pthread_getthreadid_np();
#elif defined(__APPLE__)
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    VM_ASSERT(tid < INT_MAX);
    return (int)tid;
#endif
}
#endif

#define NATIVE_MUTEX_LOCK_DEBUG 0

#if DEBUG_THREADS
    void debug_threads(FILE *f, const char *fmt, ...) {
        rb_vm_t *vm = GET_VM();
        rb_native_mutex_lock(&vm->debug_print_lock);
        va_list ap;
        va_start(ap, fmt);
        int tid = get_native_thread_id();
        if (timer_thread_tid != 0 && timer_thread_tid == tid) {
            fprintf(f, "(timer thread) ");
        } else {
            rb_execution_context_t *ec = rb_current_execution_context(false);
            if (ec) {
                rb_thread_t *th = GET_THREAD();
                if (th) {
                    if (th->has_dedicated_nt) {
                        fprintf(f, "%s(r: %d t: %d dnt: %d) ",
                            th->blocking_region_buffer ? "--" : "", rb_ractor_id(th->ractor), th->serial, (th->nt ? th->nt->serial : -1)
                        );
                    } else {
                        fprintf(f, "%s(r: %d t: %d snt: %d) ",
                            th->blocking_region_buffer ? "--" : "", rb_ractor_id(th->ractor), th->serial, (th->nt ? th->nt->serial : -1)
                        );
                    }
                }
            }
        }
        vfprintf(f, fmt, ap);
        va_end(ap);
        rb_native_mutex_unlock(&vm->debug_print_lock);
    }
#endif


static void
mutex_debug(const char *msg, void *lock)
{
    if (NATIVE_MUTEX_LOCK_DEBUG) {
        int r;
        static pthread_mutex_t dbglock = PTHREAD_MUTEX_INITIALIZER;

        if ((r = pthread_mutex_lock(&dbglock)) != 0) {exit(EXIT_FAILURE);}
        fprintf(stdout, "%s: %p\n", msg, lock);
        if ((r = pthread_mutex_unlock(&dbglock)) != 0) {exit(EXIT_FAILURE);}
    }
}

void
rb_native_mutex_lock(pthread_mutex_t *lock)
{
    int r;
    mutex_debug("lock", lock);
    if ((r = pthread_mutex_lock(lock)) != 0) {
        rb_bug_errno("pthread_mutex_lock", r);
    }
}

void
rb_native_mutex_unlock(pthread_mutex_t *lock)
{
    int r;
    mutex_debug("unlock", lock);
    if ((r = pthread_mutex_unlock(lock)) != 0) {
        rb_bug_errno("pthread_mutex_unlock", r);
    }
}

int
rb_native_mutex_trylock(pthread_mutex_t *lock)
{
    int r;
    mutex_debug("trylock", lock);
    if ((r = pthread_mutex_trylock(lock)) != 0) {
        if (r == EBUSY) {
            return EBUSY;
        }
        else {
            rb_bug_errno("pthread_mutex_trylock", r);
        }
    }
    return 0;
}

void
rb_native_mutex_initialize(pthread_mutex_t *lock)
{
    int r = pthread_mutex_init(lock, 0);
    mutex_debug("init", lock);
    if (r != 0) {
        rb_bug_errno("pthread_mutex_init", r);
    }
}

void
rb_native_mutex_destroy(pthread_mutex_t *lock)
{
    int r = pthread_mutex_destroy(lock);
    mutex_debug("destroy", lock);
    if (r != 0) {
        rb_bug_errno("pthread_mutex_destroy", r);
    }
}

void
rb_native_cond_initialize(rb_nativethread_cond_t *cond)
{
    int r = pthread_cond_init(cond, condattr_monotonic);
    if (r != 0) {
        rb_bug_errno("pthread_cond_init", r);
    }
}

void
rb_native_cond_destroy(rb_nativethread_cond_t *cond)
{
    int r = pthread_cond_destroy(cond);
    if (r != 0) {
        rb_bug_errno("pthread_cond_destroy", r);
    }
}

/*
 * In OS X 10.7 (Lion), pthread_cond_signal and pthread_cond_broadcast return
 * EAGAIN after retrying 8192 times.  You can see them in the following page:
 *
 * http://www.opensource.apple.com/source/Libc/Libc-763.11/pthreads/pthread_cond.c
 *
 * The following rb_native_cond_signal and rb_native_cond_broadcast functions
 * need to retrying until pthread functions don't return EAGAIN.
 */

void
rb_native_cond_signal(rb_nativethread_cond_t *cond)
{
    int r;
    do {
        r = pthread_cond_signal(cond);
    } while (r == EAGAIN);
    if (r != 0) {
        rb_bug_errno("pthread_cond_signal", r);
    }
}

void
rb_native_cond_broadcast(rb_nativethread_cond_t *cond)
{
    int r;
    do {
        r = pthread_cond_broadcast(cond);
    } while (r == EAGAIN);
    if (r != 0) {
        rb_bug_errno("rb_native_cond_broadcast", r);
    }
}

void
rb_native_cond_wait(rb_nativethread_cond_t *cond, pthread_mutex_t *mutex)
{
    int r = pthread_cond_wait(cond, mutex);
    if (r != 0) {
        rb_bug_errno("pthread_cond_wait", r);
    }
}

static int
native_cond_timedwait(rb_nativethread_cond_t *cond, pthread_mutex_t *mutex, const rb_hrtime_t *abs)
{
    int r;
    struct timespec ts;

    /*
     * An old Linux may return EINTR. Even though POSIX says
     *   "These functions shall not return an error code of [EINTR]".
     *   http://pubs.opengroup.org/onlinepubs/009695399/functions/pthread_cond_timedwait.html
     * Let's hide it from arch generic code.
     */
    do {
        rb_hrtime2timespec(&ts, abs);
        r = pthread_cond_timedwait(cond, mutex, &ts);
    } while (r == EINTR);

    if (r != 0 && r != ETIMEDOUT) {
        rb_bug_errno("pthread_cond_timedwait", r);
    }

    return r;
}

static rb_hrtime_t
native_cond_timeout(rb_nativethread_cond_t *cond, const rb_hrtime_t rel)
{
    if (condattr_monotonic) {
        return rb_hrtime_add(rb_hrtime_now(), rel);
    }
    else {
        struct timespec ts;

        rb_timespec_now(&ts);
        return rb_hrtime_add(rb_timespec2hrtime(&ts), rel);
    }
}

void
rb_native_cond_timedwait(rb_nativethread_cond_t *cond, pthread_mutex_t *mutex, unsigned long msec)
{
    rb_hrtime_t hrmsec = native_cond_timeout(cond, RB_HRTIME_PER_MSEC * msec);
    native_cond_timedwait(cond, mutex, &hrmsec);
}

// thread scheduling

static rb_internal_thread_event_hook_t *rb_internal_thread_event_hooks = NULL;
static void rb_thread_execute_hooks(rb_event_flag_t event, rb_thread_t *th);

#if 0
static const char *
event_name(rb_event_flag_t event)
{
    switch (event) {
      case RUBY_INTERNAL_THREAD_EVENT_STARTED:
        return "STARTED";
      case RUBY_INTERNAL_THREAD_EVENT_READY:
        return "READY";
      case RUBY_INTERNAL_THREAD_EVENT_RESUMED:
        return "RESUMED";
      case RUBY_INTERNAL_THREAD_EVENT_SUSPENDED:
        return "SUSPENDED";
      case RUBY_INTERNAL_THREAD_EVENT_EXITED:
        return "EXITED";
    }
    return "no-event";
}

#define RB_INTERNAL_THREAD_HOOK(event, th) \
    if (UNLIKELY(rb_internal_thread_event_hooks)) { \
        fprintf(stderr, "[thread=%"PRIxVALUE"] %s in %s (%s:%d)\n", th->self, event_name(event), __func__, __FILE__, __LINE__); \
        rb_thread_execute_hooks(event, th); \
    }
#else
#define RB_INTERNAL_THREAD_HOOK(event, th) if (UNLIKELY(rb_internal_thread_event_hooks)) { rb_thread_execute_hooks(event, th); }
#endif

static rb_serial_t current_fork_gen = 1; /* We can't use GET_VM()->fork_gen */

#if defined(SIGVTALRM) && !defined(__EMSCRIPTEN__)
#  define USE_UBF_LIST 1
#endif

static void threadptr_trap_interrupt(rb_thread_t *);

#ifdef HAVE_SCHED_YIELD
#define native_thread_yield() (void)sched_yield()
#else
#define native_thread_yield() ((void)0)
#endif

/* 100ms.  10ms is too small for user level thread scheduling
 * on recent Linux (tested on 2.6.35)
 */
#define TIME_QUANTUM_MSEC (100)
#define TIME_QUANTUM_USEC (TIME_QUANTUM_MSEC * 1000)
#define TIME_QUANTUM_NSEC (TIME_QUANTUM_USEC * 1000)
#define TIME_TIMEOUT_MSEC (10)
#define TIME_TIMEOUT_USEC (TIME_TIMEOUT_MSEC * 1000)
#define TIME_TIMEOUT_MAX_MS (2500)

static void native_thread_dedicated_inc(rb_vm_t *vm, rb_ractor_t *cr, struct rb_native_thread *nt);
static void native_thread_dedicated_dec(rb_vm_t *vm, rb_ractor_t *cr, struct rb_native_thread *nt);
static void native_thread_assign(struct rb_native_thread *nt, rb_thread_t *th);
static void native_thread_set_blocked(struct rb_native_thread *nt, enum nt_blocked_reason reason);
static void native_thread_set_blocked_unchecked(struct rb_native_thread *nt, enum nt_blocked_reason reason);
static void native_thread_set_unblocked(struct rb_native_thread *nt);

static void ractor_sched_enq(rb_vm_t *vm, rb_ractor_t *r, bool allow_already_in);
static void timer_thread_wakeup(void);
static void timer_thread_wakeup_locked(rb_vm_t *vm, bool needs_lock);
static void timer_thread_wakeup_force(void);
static void thread_sched_switch(rb_thread_t *cth, rb_thread_t *next_th);
static void coroutine_transfer0(struct coroutine_context *transfer_from,
                                struct coroutine_context *transfer_to, bool to_dead);
static void coroutine_transfer_thread(struct coroutine_context *transfer_from,
                                      struct coroutine_context *transfer_to, rb_thread_t *thread, struct rb_native_thread *nt,
                                      bool to_dead);

#define thread_sched_dump(s) thread_sched_dump_(__FILE__, __LINE__, s)

static bool
th_has_dedicated_nt(const rb_thread_t *th)
{
    // TODO: th->has_dedicated_nt
    return th->nt->is_permanent_dnt || th->nt->dedicated > 0;
}

RBIMPL_ATTR_MAYBE_UNUSED()
static void
thread_sched_dump_(const char *file, int line, struct rb_thread_sched *sched)
{
    fprintf(stderr, "@%s:%d running:%d\n", file, line, sched->running ? (int)sched->running->serial : -1);
    rb_thread_t *th;
    int i = 0;
    ccan_list_for_each(&sched->readyq, th, sched.node.readyq) {
        i++; if (i>10) rb_bug("too many");
        fprintf(stderr, "  ready:%d (%sNT:%d)\n", th->serial,
                th->nt ? (th->nt->dedicated ? "D" : "S") : "x",
                th->nt ? (int)th->nt->serial : -1);
    }
}

#define ractor_sched_dump(s) ractor_sched_dump_(__FILE__, __LINE__, s)

RBIMPL_ATTR_MAYBE_UNUSED()
static void
ractor_sched_dump_(const char *file, int line, rb_vm_t *vm)
{
    rb_ractor_t *r;

    debug_threads(stderr, "ractor_sched_dump:\n");

    int i = 0;
    ccan_list_for_each(&vm->ractor.sched.grq, r, threads.sched.grq_node) {
        i++;
        debug_threads(stderr, "  %d ready r:%d\n", i, rb_ractor_id(r));
    }
}

#define thread_sched_lock(a, b) thread_sched_lock_(a, b, __FILE__, __LINE__)
#define thread_sched_unlock(a, b) thread_sched_unlock_(a, b, __FILE__, __LINE__)

static void
thread_sched_lock_(struct rb_thread_sched *sched, rb_thread_t *th, const char *file, int line)
{
    debug_threads(stderr, "Locking thread sched lock (before), owner_th:%d\n", th ? th->serial : -1);
    rb_native_mutex_lock(&sched->lock_);
    debug_threads(stderr, "Locking thread sched lock (after), owner_th:%d\n", th ? th->serial : -1);

#if VM_CHECK_MODE
    RUBY_DEBUG_LOG2(file, line, "th:%u prev_owner:%u", rb_th_serial(th), rb_th_serial(sched->lock_owner));
    VM_ASSERT(sched->lock_owner == NULL);
    sched->lock_owner = th;
#else
    RUBY_DEBUG_LOG2(file, line, "th:%u", rb_th_serial(th));
#endif
    sched->locked = true;
}

static void
thread_sched_unlock_(struct rb_thread_sched *sched, rb_thread_t *th, const char *file, int line)
{
    RUBY_DEBUG_LOG2(file, line, "th:%u", rb_th_serial(th));

    VM_ASSERT(sched->locked);
#if VM_CHECK_MODE
    if (th) {
        VM_ASSERT(sched->lock_owner == th);
    }
    sched->lock_owner = NULL;
#endif

    sched->locked = false;
    rb_native_mutex_unlock(&sched->lock_);
    debug_threads(stderr, "Unlocking thread sched lock, owner_th:%d\n", th ? th->serial : -1);
}

static void
thread_sched_set_lock_owner(struct rb_thread_sched *sched, rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%u", rb_th_serial(th));

#if VM_CHECK_MODE > 0
    sched->lock_owner = th;
#endif
}

static void
ASSERT_thread_sched_locked(struct rb_thread_sched *sched, rb_thread_t *th)
{
    VM_ASSERT(rb_native_mutex_trylock(&sched->lock_) == EBUSY);

#if VM_CHECK_MODE
    if (th) {
        VM_ASSERT(sched->lock_owner == th);
    }
    else {
        VM_ASSERT(sched->lock_owner != NULL);
    }
#endif
}

#define ractor_sched_lock(a, b) ractor_sched_lock_(a, b, __FILE__, __LINE__)
#define ractor_sched_unlock(a, b) ractor_sched_unlock_(a, b, __FILE__, __LINE__)

RBIMPL_ATTR_MAYBE_UNUSED()
static unsigned int
rb_ractor_serial(const rb_ractor_t *r) {
    if (r) {
        return rb_ractor_id(r);
    }
    else {
        return 0;
    }
}

static void
ractor_sched_set_locked(rb_vm_t *vm, rb_ractor_t *cr)
{
    /*VM_ASSERT(vm->ractor.sched.lock_owner == NULL);*/
    /*VM_ASSERT(vm->ractor.sched.locked == false);*/
    /*vm->ractor.sched.lock_owner = cr;*/
    /*vm->ractor.sched.locked = true;*/
#if VM_CHECK_MODE > 0
    VM_ASSERT(vm->ractor.sched.lock_owner == NULL);
    VM_ASSERT(vm->ractor.sched.locked == false);

    vm->ractor.sched.lock_owner = cr;
    vm->ractor.sched.locked = true;
#endif
}

static void
ractor_sched_set_unlocked(rb_vm_t *vm, rb_ractor_t *cr)
{
#if VM_CHECK_MODE > 0
    VM_ASSERT(vm->ractor.sched.locked);
    VM_ASSERT(vm->ractor.sched.lock_owner == cr);

    vm->ractor.sched.locked = false;
    vm->ractor.sched.lock_owner = NULL;
#endif
}

static void
ractor_sched_lock_(rb_vm_t *vm, rb_ractor_t *cr, const char *file, int line)
{
    debug_threads(stderr, "Locking ractor sched lock r:%d before\n",
        cr ? rb_ractor_id(cr) : -1
    );
    rb_native_mutex_lock(&vm->ractor.sched.lock);
    debug_threads(stderr, "Locking ractor sched lock r:%d after\n",
        cr ? rb_ractor_id(cr) : -1
    );

#if VM_CHECK_MODE
    RUBY_DEBUG_LOG2(file, line, "cr:%u prev_owner:%u", rb_ractor_serial(cr), rb_ractor_serial(vm->ractor.sched.lock_owner));
#else
    RUBY_DEBUG_LOG2(file, line, "cr:%u", rb_ractor_serial(cr));
#endif

    ractor_sched_set_locked(vm, cr);
}

static void
ractor_sched_unlock_(rb_vm_t *vm, rb_ractor_t *cr, const char *file, int line)
{
    RUBY_DEBUG_LOG2(file, line, "cr:%u", rb_ractor_serial(cr));

    ractor_sched_set_unlocked(vm, cr);
    rb_native_mutex_unlock(&vm->ractor.sched.lock);
    debug_threads(stderr, "Unlocking ractor sched lock r:%d\n",
        cr ? rb_ractor_id(cr) : -1
    );
}

static void
ASSERT_ractor_sched_locked(rb_vm_t *vm, rb_ractor_t *cr)
{
    VM_ASSERT(rb_native_mutex_trylock(&vm->ractor.sched.lock) == EBUSY);
    VM_ASSERT(vm->ractor.sched.locked);
    VM_ASSERT(cr == NULL || vm->ractor.sched.lock_owner == cr);
}

RBIMPL_ATTR_MAYBE_UNUSED()
static bool
ractor_sched_running_threads_contain_p(rb_vm_t *vm, rb_thread_t *th)
{
    rb_thread_t *rth;
    ccan_list_for_each(&vm->ractor.sched.running_threads, rth, sched.node.running_threads) {
        if (rth == th) return true;
    }
    return false;
}

RBIMPL_ATTR_MAYBE_UNUSED()
static unsigned int
ractor_sched_running_threads_size(rb_vm_t *vm)
{
    rb_thread_t *th;
    unsigned int i = 0;
    ccan_list_for_each(&vm->ractor.sched.running_threads, th, sched.node.running_threads) {
        i++;
    }
    return i;
}

RBIMPL_ATTR_MAYBE_UNUSED()
static unsigned int
ractor_sched_timeslice_threads_size(rb_vm_t *vm)
{
    rb_thread_t *th;
    unsigned int i = 0;
    ccan_list_for_each(&vm->ractor.sched.timeslice_threads, th, sched.node.timeslice_threads) {
        i++;
    }
    return i;
}

RBIMPL_ATTR_MAYBE_UNUSED()
static bool
ractor_sched_timeslice_threads_contain_p(rb_vm_t *vm, rb_thread_t *th)
{
    rb_thread_t *rth;
    ccan_list_for_each(&vm->ractor.sched.timeslice_threads, rth, sched.node.timeslice_threads) {
        if (rth == th) return true;
    }
    return false;
}

static void ractor_sched_barrier_join_signal_locked(rb_vm_t *vm);
static void ractor_sched_barrier_join_wait_locked(rb_vm_t *vm, rb_thread_t *th);

// setup timeslice signals by the timer thread.
// Luke: TODO: avoid grabbing ractor_sched_lock
static void
thread_sched_setup_running_threads(struct rb_thread_sched *sched, rb_ractor_t *cr, rb_vm_t *vm,
                                   rb_thread_t *add_th, rb_thread_t *del_th, rb_thread_t *add_timeslice_th)
{
#if USE_RUBY_DEBUG_LOG
    unsigned int prev_running_cnt = vm->ractor.sched.running_cnt;
#endif

    rb_thread_t *del_timeslice_th;

    if (del_th && sched->is_running_timeslice) {
        del_timeslice_th = del_th;
        // Luke: weird
        sched->is_running_timeslice = false; // TODO: figure out what this is
    }
    else {
        del_timeslice_th = NULL;
    }

    RUBY_DEBUG_LOG("+:%u -:%u +ts:%u -ts:%u",
                   rb_th_serial(add_th), rb_th_serial(del_th),
                   rb_th_serial(add_timeslice_th), rb_th_serial(del_timeslice_th));
    // TODO: try to avoid ractor sched lock when is_permanent_dnt
    bool needs_lock = true;
    if (needs_lock) {
        ractor_sched_lock(vm, cr);
    }
    {
        // update running_threads
        if (del_th) {
            debug_threads(stderr, "thread_sched_setup_running_threads: del_th: %d\n", del_th->serial);
            VM_ASSERT(ractor_sched_running_threads_contain_p(vm, del_th));
            VM_ASSERT(del_timeslice_th != NULL ||
                      !ractor_sched_timeslice_threads_contain_p(vm, del_th));

            ccan_list_del_init(&del_th->sched.node.running_threads);
            vm->ractor.sched.running_cnt--;
            VM_ASSERT(vm->ractor.sched.running_cnt >= 0);

            if (UNLIKELY(vm->ractor.sched.barrier_waiting)) {
                debug_threads(stderr, "thread_sched_setup_running_threads: del_th: barrier_join_signal_locked\n");
                ractor_sched_barrier_join_signal_locked(vm);
            }
            // Luke: weird
            sched->is_running = false;
        }

        if (add_th) {
            debug_threads(stderr, "thread_sched_setup_running_threads: add_th: %d\n", add_th->serial);
            while (UNLIKELY(vm->ractor.sched.barrier_waiting)) {
                RUBY_DEBUG_LOG("barrier-wait");
                debug_threads(stderr, "thread_sched_setup_running_threads: add_th: barrier_waiting\n");

                ractor_sched_barrier_join_signal_locked(vm);
                ractor_sched_barrier_join_wait_locked(vm, add_th);
            }

            VM_ASSERT(!ractor_sched_running_threads_contain_p(vm, add_th));
            VM_ASSERT(!ractor_sched_timeslice_threads_contain_p(vm, add_th));

            ccan_list_add(&vm->ractor.sched.running_threads, &add_th->sched.node.running_threads);
            vm->ractor.sched.running_cnt++;
            sched->is_running = true;
            VM_ASSERT(!vm->ractor.sched.barrier_waiting);
        }

        if (add_timeslice_th) {
            // update timeslice threads
            int was_empty = ccan_list_empty(&vm->ractor.sched.timeslice_threads);
            debug_threads(stderr, "thread_sched_setup_running_threads: add_timeslice_th:%d (wake timer:%d)\n",
                add_timeslice_th->serial, was_empty ? 1 : 0
            );
            VM_ASSERT(!ractor_sched_timeslice_threads_contain_p(vm, add_timeslice_th));
            ccan_list_add(&vm->ractor.sched.timeslice_threads, &add_timeslice_th->sched.node.timeslice_threads);
            sched->is_running_timeslice = true;
            if (was_empty) {
                timer_thread_wakeup_locked(vm, needs_lock); // wakeup the timer thread!
            }
        }

        if (del_timeslice_th) {
            debug_threads(stderr, "thread_sched_setup_running_threads: del_timeslice_th: %d\n", del_timeslice_th->serial);
            if (ractor_sched_timeslice_threads_contain_p(vm, del_timeslice_th)) {
                ccan_list_del_init(&del_timeslice_th->sched.node.timeslice_threads);
            }
            // TODO: put this back
            /*VM_ASSERT(ractor_sched_timeslice_threads_contain_p(vm, del_timeslice_th));*/
        }

        VM_ASSERT(ractor_sched_running_threads_size(vm) == vm->ractor.sched.running_cnt);
        VM_ASSERT(ractor_sched_timeslice_threads_size(vm) <= vm->ractor.sched.running_cnt);
    }
    if (needs_lock) {
        ractor_sched_unlock(vm, cr);
    }

    if (add_th && !del_th && UNLIKELY(vm->ractor.sync.lock_owner != NULL)) {
        // it can be after barrier synchronization by another ractor
        rb_thread_t *lock_owner = NULL;
#if VM_CHECK_MODE
        lock_owner = sched->lock_owner;
#endif
        debug_threads(stderr, "thread_sched_setup_running_threads: running UNLOCK/LOCK\n");
        thread_sched_unlock(sched, lock_owner);
        {
            RB_VM_LOCK_ENTER();
            RB_VM_LOCK_LEAVE();
        }
        thread_sched_lock(sched, lock_owner);
    }

    //RUBY_DEBUG_LOG("+:%u -:%u +ts:%u -ts:%u run:%u->%u",
    //               rb_th_serial(add_th), rb_th_serial(del_th),
    //               rb_th_serial(add_timeslice_th), rb_th_serial(del_timeslice_th),
    RUBY_DEBUG_LOG("run:%u->%u", prev_running_cnt, vm->ractor.sched.running_cnt);
}

// Preconditions:
//   sched is locked
//   sched->running == th
static void
thread_sched_add_running_thread(struct rb_thread_sched *sched, rb_thread_t *th)
{
    ASSERT_thread_sched_locked(sched, th);
    VM_ASSERT(sched->running == th);

    rb_vm_t *vm = th->vm;
    // Luke: This will only add the thread to timeslice_threads if sched->readyq is empty
    thread_sched_setup_running_threads(sched, th->ractor, vm, th, NULL, ccan_list_empty(&sched->readyq) ? NULL : th);
}

static void
thread_sched_del_running_thread(struct rb_thread_sched *sched, rb_thread_t *th)
{
    ASSERT_thread_sched_locked(sched, th);

    rb_vm_t *vm = th->vm;
    thread_sched_setup_running_threads(sched, th->ractor, vm, NULL, th, NULL);
}

void
rb_add_running_thread(rb_thread_t *th)
{
    struct rb_thread_sched *sched = TH_SCHED(th);

    thread_sched_lock(sched, th);
    {
        thread_sched_add_running_thread(sched, th);
    }
    thread_sched_unlock(sched, th);
}

void
rb_del_running_thread(rb_thread_t *th)
{
    struct rb_thread_sched *sched = TH_SCHED(th);

    thread_sched_lock(sched, th);
    {
        thread_sched_del_running_thread(sched, th);
    }
    thread_sched_unlock(sched, th);
}

// NOTE:
//   sched does NOT have to be current ractor's sched
//   th can be NULL
// setup current or next running thread
// sched->running should be set only on this function.
//
// if th is NULL, there is no running threads.
static void
thread_sched_set_running(struct rb_thread_sched *sched, rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%u->th:%u", rb_th_serial(sched->running), rb_th_serial(th));
    VM_ASSERT(sched->running != th);
    rb_ractor_t *r = 0;
    if (th) {
        r = th->ractor;
    } else {
        r = sched->running->ractor;
    }
    VM_ASSERT(r);

    debug_threads(stderr, "thread_sched_set_running r:%d th_from:%d th_to:%d\n",
        rb_ractor_id(r), sched->running ? sched->running->serial : -1,
        th ? th->serial : -1
    );
    sched->running = th;
}

RBIMPL_ATTR_MAYBE_UNUSED()
static bool
thread_sched_readyq_contain_p(struct rb_thread_sched *sched, rb_thread_t *th)
{
    rb_thread_t *rth;
    ccan_list_for_each(&sched->readyq, rth, sched.node.readyq) {
        if (rth == th) return true;
    }
    return false;
}

// deque thread from the ready queue.
// if the ready queue is empty, return NULL.
//
// return deque'ed running thread (or NULL).
static rb_thread_t *
thread_sched_deq(struct rb_thread_sched *sched)
{
    ASSERT_thread_sched_locked(sched, NULL);
    rb_thread_t *next_th;

    VM_ASSERT(sched->running != NULL);

    if (ccan_list_empty(&sched->readyq)) {
        next_th = NULL;
    }
    else {
        next_th = ccan_list_pop(&sched->readyq, rb_thread_t, sched.node.readyq);

        VM_ASSERT(sched->readyq_cnt > 0);
        sched->readyq_cnt--;
        ccan_list_node_init(&next_th->sched.node.readyq);
    }

    RUBY_DEBUG_LOG("next_th:%u readyq_cnt:%d", rb_th_serial(next_th), sched->readyq_cnt);

    return next_th;
}

static void
thread_sched_enq_check_simple(struct rb_thread_sched *sched, rb_thread_t *th)
{
    ASSERT_thread_sched_locked(sched, NULL);
    if (!thread_sched_readyq_contain_p(sched, th)) {
        ccan_list_add_tail(&sched->readyq, &th->sched.node.readyq);
        sched->readyq_cnt++;
    }
}

// enqueue ready thread to the ractor thread queue
static void
thread_sched_enq(struct rb_thread_sched *sched, rb_thread_t *ready_th, bool add_tail)
{
    ASSERT_thread_sched_locked(sched, NULL);
    RUBY_DEBUG_LOG("ready_th:%u readyq_cnt:%d", rb_th_serial(ready_th), sched->readyq_cnt);

    VM_ASSERT(ready_th != NULL);
    VM_ASSERT(sched->running != NULL);
    VM_ASSERT(TH_SCHED(ready_th) == sched);
    VM_ASSERT(!thread_sched_readyq_contain_p(sched, ready_th));

    if (sched->is_running) {
        debug_threads(stderr, "thread_sched_enq: ready_th:%d readyq_cnt:%d (before incr) sched->is_running\n",
            ready_th->serial,
            sched->readyq_cnt
        );
        if (ccan_list_empty(&sched->readyq)) {
            VM_ASSERT(sched->readyq_cnt == 0);
            debug_threads(stderr, "thread_sched_enq: empty sched->readyq\n");
            // add sched->running to timeslice
            VM_ASSERT(sched->running != ready_th);
            thread_sched_setup_running_threads(sched, ready_th->ractor, ready_th->vm, NULL, NULL, sched->running);
        } else {
            // Luke: I'm not sure when this would happen!?
            debug_threads(stderr, "thread_sched_enq: NON-EMPTY sched->readyq ???\n");
            VM_ASSERT(sched->readyq_cnt > 0);
        }
    }
    else {
        // Luke: I'm not sure when this would happen!?
        debug_threads(stderr, "thread_sched_enq: ready_th:%d readyq_cnt:%d !sched->is_running???\n",
            ready_th->serial,
            sched->readyq_cnt
        );
        VM_ASSERT(!ractor_sched_timeslice_threads_contain_p(ready_th->vm, sched->running));
    }

    if (add_tail) {
        ccan_list_add_tail(&sched->readyq, &ready_th->sched.node.readyq);
    } else {
        // add to front of list
        ccan_list_add(&sched->readyq, &ready_th->sched.node.readyq);
    }
    sched->readyq_cnt++;
}

static bool
thread_sched_wait_running_turn(struct rb_thread_sched *sched, rb_thread_t *th, bool can_direct_transfer);

/*
 * Preconditions:
 *   sched is locked
 *   sched->running == next_th
 * Arguments:
 *   next_th can be NULL
 *   next_th does NOT have to be thread of current ractor
 */
// DNT: kick condvar
// SNT: TODO
static void
thread_sched_wakeup_running_thread(struct rb_thread_sched *sched, rb_thread_t *prev_th, rb_thread_t *next_th, bool will_switch)
{
    ASSERT_thread_sched_locked(sched, NULL);
    VM_ASSERT(sched->running == next_th);

    if (next_th) {
        if (next_th->nt) {
            if (th_has_dedicated_nt(next_th)) {
                RUBY_DEBUG_LOG("pinning th:%u", next_th->serial);
                if (next_th->nt->blocked_reason == NT_BLOCKED_DEDICATED_READYQ) {
                    debug_threads(stderr, "thread_sched_wakeup_running_thread: next_th:%d, next_th->nt, signalling nt:%d (nt->cond.readyq)\n",
                        next_th->serial, next_th->nt->serial
                    );
                    rb_native_cond_signal(&next_th->nt->cond.readyq);
                } else {
                    debug_threads(stderr, "thread_sched_wakeup_running_thread: not signalling (timed out) next_th:%d, next_th->nt:%d\n",
                        next_th->serial, next_th->nt->serial
                    );
                }
            }
            else {
                // TODO
                RUBY_DEBUG_LOG("th:%u is already running.", next_th->serial);
                debug_threads(stderr, "thread_sched_wakeup_running_thread: th:%d is already running on nt:%d\n",
                    next_th->serial, next_th->nt->serial
                );
            }
        }
        else {
            if (will_switch) {
                RUBY_DEBUG_LOG("th:%u (do nothing)", rb_th_serial(next_th));
                debug_threads(stderr, "thread_sched_wakeup_running_thread: next_th:%d, !next_th->nt prev_th:%d will_switch (do nothing)\n",
                    next_th->serial, prev_th ? prev_th->serial : -1
                );
                // Luke: I'm not sure if this will work!
                /*thread_sched_add_running_thread(sched, next_th);*/
                /*if (!prev_th) {*/
                    /*rb_thread_t *prev_lock_owner = sched->lock_owner; // todo: guard with defined*/
                    /*thread_sched_set_lock_owner(sched, NULL);*/
                    /*{*/
                        /*rb_ractor_set_current_ec(next_th->ractor, NULL);*/
                        /*debug_threads(stderr, "thread_sched_wait_running_turn: can_direct_transfer from to:%d to next_th:%d\n",*/
                                      /*prev_th ? prev_th->serial : -1, next_th->serial*/
                                      /*);*/
                        /*// note: coroutine_transfer with thread sched lock*/
                        /*thread_sched_switch(prev_th, next_th);*/
                    /*}*/
                    /*thread_sched_set_lock_owner(sched, prev_lock_owner);*/
                /*} else {*/
                    /*rb_thread_t *prev_owner = sched->lock_owner;*/
                    /*thread_sched_set_lock_owner(sched, next_th);*/
                    /*thread_sched_wait_running_turn(sched, next_th, true);*/
                    /*thread_sched_set_lock_owner(sched, prev_owner);*/
                /*}*/
            }
            else {
                RUBY_DEBUG_LOG("th:%u (enq)", rb_th_serial(next_th));
                debug_threads(stderr, "thread_sched_wakeup_running_thread: next_th:%d, !will_switch, ractor_sched_enq\n",
                    next_th->serial
                );
                ractor_sched_enq(next_th->vm, next_th->ractor, true);
            }
        }
    }
    else {
        RUBY_DEBUG_LOG("no waiting threads%s", "");
        debug_threads(stderr, "thread_sched_wakeup_running_thread: no waiting threads (next_th == NULL)\n");
    }
}

/*
 * Arguments:
 *   th != NULL
 *   th->ractor does NOT always equal GET_RACTOR()
 * Preconditions:
 *   sched is locked
 *   sched->running != th
 *   sched readyq does NOT contain th
 */
// waiting -> ready (locked)
static void
thread_sched_to_ready_common(struct rb_thread_sched *sched, rb_thread_t *th, bool wakeup, bool will_switch)
{
    RUBY_DEBUG_LOG("th:%u running:%u redyq_cnt:%d", rb_th_serial(th), rb_th_serial(sched->running), sched->readyq_cnt);

    VM_ASSERT(th);
    VM_ASSERT(sched->running != th);
    VM_ASSERT(!thread_sched_readyq_contain_p(sched, th));
    RB_INTERNAL_THREAD_HOOK(RUBY_INTERNAL_THREAD_EVENT_READY, th);
    // Luke: I think this is needed for Ractors!
    /*if (th->ractor != GET_RACTOR()) {*/
        /*will_switch = false;*/
    /*}*/

    if (sched->running == NULL) {
        debug_threads(stderr, "thread_sched_to_ready_common th:%d sched->running == NULL, wakeup:%d, will_switch:%d\n",
            th->serial, wakeup, will_switch
        );
        thread_sched_set_running(sched, th);
        if (wakeup) {
            thread_sched_wakeup_running_thread(sched, NULL, th, will_switch);
        }
        /*thread_sched_set_running(TH_SCHED(th), th);*/
        /*thread_sched_enq(sched, th);*/
        if (will_switch) {
        }
        if (wakeup) {
            /*ractor_sched_lock(th->vm, th->ractor);*/
            /*rb_ractor_t *tr;*/
            /*bool found = false;*/
            /*ccan_list_for_each(&th->vm->ractor.sched.grq, tr, threads.sched.grq_node) {*/
                /*if (th->ractor == tr) {*/
                    /*found = true;*/
                /*}*/
            /*}*/
            /*ractor_sched_unlock(th->vm, th->ractor);*/
            /*if (!found) {*/
                /*ractor_sched_enq(th->vm, th->ractor);*/
            /*}*/
        }
    }
    else {
        debug_threads(stderr, "thread_sched_to_ready_common th:%d sched->running != NULL, wakeup:%d, will_switch:%d\n", th->serial, wakeup, will_switch);
        thread_sched_enq(sched, th, true);
    }
}

// Arguments:
//   th != NULL
//
// waiting -> ready
//
// `th` had became "waiting" state by `thread_sched_to_waiting`
// and `thread_sched_to_ready` enqueue `th` to the thread ready queue.
RBIMPL_ATTR_MAYBE_UNUSED()
static void
thread_sched_to_ready(struct rb_thread_sched *sched, rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%u", rb_th_serial(th));
    debug_threads(stderr, "thread_sched_to_ready: setting th:%d as ready for r:%d\n", rb_th_serial(th), rb_ractor_id(th->ractor));

    thread_sched_lock(sched, th);
    {
        thread_sched_to_ready_common(sched, th, true, false);
    }
    thread_sched_unlock(sched, th);
}

// Preconditions:
//   sched(th) is locked
//
// wait until sched->running is `th`.
static bool
thread_sched_wait_running_turn(struct rb_thread_sched *sched, rb_thread_t *th, bool can_direct_transfer)
{
    RUBY_DEBUG_LOG("th:%u", rb_th_serial(th));

    VM_ASSERT(th);
    ASSERT_thread_sched_locked(sched, th);
    VM_ASSERT(rb_current_ec_noinline() == th->ec);
    VM_ASSERT(th == rb_ec_thread_ptr(rb_current_ec_noinline()));
    bool timed_out = false;

    if (th != sched->running) {
        // already deleted from running threads
        // VM_ASSERT(!ractor_sched_running_threads_contain_p(th->vm, th)); // need locking

        // wait for execution right
        rb_thread_t *next_th;
        while ((next_th = sched->running) != th) { // NOTE: next_th can be NULL
            if (th_has_dedicated_nt(th)) {
                RUBY_DEBUG_LOG("(nt) sleep th:%u running:%u", rb_th_serial(th), rb_th_serial(sched->running));

                debug_threads(stderr, "thread_sched_wait_running_turn: r:%d th:%d next_th:%d nt:%d (dnt?:%d) cond_wait(nt->cond.readyq) before\n",
                    rb_ractor_id(th->ractor), th->serial, next_th ? next_th->serial : -1, th->nt->serial, th->nt->is_permanent_dnt
                );
                native_thread_set_blocked(th->nt, NT_BLOCKED_DEDICATED_READYQ);
                thread_sched_set_lock_owner(sched, NULL);
                {
                    RUBY_DEBUG_LOG("nt:%d cond:%p", th->nt->serial, &th->nt->cond.readyq);
                    rb_native_cond_wait(&th->nt->cond.readyq, &sched->lock_); // lock then unlock
                }
                thread_sched_set_lock_owner(sched, th);
                native_thread_set_unblocked(th->nt);
                debug_threads(stderr, "thread_sched_wait_running_turn: r:%d th:%d next_th:%d nt:%d (dnt?:%d) cond_wait after (wakeup, success:%d)\n",
                    rb_ractor_id(th->ractor), th->serial, next_th ? next_th->serial : -1, th->nt->serial, th->nt->is_permanent_dnt, sched->running == th ? 1 : 0
                );

                RUBY_DEBUG_LOG("(nt) wakeup %s", sched->running == th ? "success" : "failed");
                if (th == sched->running) {
                    // th->ractor->threads.running_ec = th->ec, so basically switch so GET_RACTOR() works
                    rb_ractor_thread_switch(th->ractor, th);
                }
            }
            else {
                // search another ready thread
                if (can_direct_transfer &&
                    (next_th = sched->running) != NULL &&
                    !next_th->nt // next_th is ready to run and has no dedicated nt
                    ) {

                    RUBY_DEBUG_LOG("th:%u->%u (direct)", rb_th_serial(th), rb_th_serial(next_th));

                    thread_sched_set_lock_owner(sched, NULL);
                    {
                        debug_threads(stderr, "thread_sched_wait_running_turn: can_direct_transfer (thread_sched_switch) from to:%d to next_th:%d\n",
                            th->serial, next_th->serial
                        );
                        rb_ractor_set_current_ec(th->ractor, NULL);
                        // NOTE: coroutine_transfer WITH thread sched lock
                        VM_ASSERT(th->ractor == next_th->ractor);
                        thread_sched_switch(th, next_th);
                    }
                    debug_threads(stderr,
                        "thread_sched_wait_running_turn: can_direct_transfer from to:%d to next_th:%d, AFTER transfer\n",
                        th->serial, next_th->serial
                    );
                    thread_sched_set_lock_owner(sched, th);
                }
                else {
                    debug_threads(stderr,
                        "thread_sched_wait_running_turn ractor scheduling r:%d th:%d next_th (sched->running):%d next_th->nt:%d\n",
                        rb_ractor_id(th->ractor), th->serial, next_th ? next_th->serial : -1,
                        next_th && next_th->nt ? next_th->nt->serial : -1
                    );
                    // search another ready ractor
                    struct rb_native_thread *nt = th->nt;
                    native_thread_assign(NULL, th);

                    RUBY_DEBUG_LOG("th:%u->%u (ractor scheduling)", rb_th_serial(th), rb_th_serial(next_th));

                    thread_sched_set_lock_owner(sched, NULL);
                    {
                        debug_threads(stderr,
                            "thread_sched_wait_running_turn r:%d th:%d calling coroutine_transfer_thread\n",
                            rb_ractor_id(th->ractor),
                            th->serial
                        );
                        rb_ractor_set_current_ec(th->ractor, NULL);
                        // Jumps back to ractor_sched_deq() loop in nt_start
                        // NOTE: coroutine_transfer WITH thread sched lock
                        coroutine_transfer_thread(th->sched.context, nt->nt_context, th, nt, false);
                    }
                    thread_sched_set_lock_owner(sched, th);
                    debug_threads(stderr,
                        "thread_sched_wait_running_turn r:%d th:%d coroutine_transfer_thread AFTER\n",
                        rb_ractor_id(th->ractor),
                        th->serial
                    );
                }

                VM_ASSERT(rb_current_ec_noinline() == th->ec);
            }
        }
        // next_th == th

        VM_ASSERT(th->nt != NULL);
        VM_ASSERT(rb_current_ec_noinline() == th->ec);
        VM_ASSERT(th->sched.waiting_reason.flags == thread_sched_waiting_none);

        // add th to running threads
        thread_sched_add_running_thread(sched, th);
    } else {
        debug_threads(stderr, "thread_sched_wait_running_turn th:%d == sched->running, no need to wait\n",
            th->serial
        );
    }

    // VM_ASSERT(ractor_sched_running_threads_contain_p(th->vm, th)); need locking
    RB_INTERNAL_THREAD_HOOK(RUBY_INTERNAL_THREAD_EVENT_RESUMED, th);
    debug_threads(stderr, "thread_sched_wait_running_turn th:%d RESUMED\n", th->serial);
    return timed_out;
}

// Run after it was set to waiting by the VM for some dedicated task like waitpid
// Arguments:
//   th != NULL
// Preconditions:
//   sched is locked
//   sched->running != th
//   GET_THREAD() == th
//
// waiting -> ready -> running (locked)
static void
thread_sched_to_running_common(struct rb_thread_sched *sched, rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%u dedicated:%d", rb_th_serial(th), th_has_dedicated_nt(th));
    debug_threads(stderr, "thread_sched_to_running_common: r:%d th:%d\n", rb_ractor_id(th->ractor), th->serial);

    VM_ASSERT(sched->running != th);
    VM_ASSERT(th_has_dedicated_nt(th));
    VM_ASSERT(GET_THREAD() == th);

    native_thread_dedicated_dec(th->vm, th->ractor, th->nt);

    // waiting -> ready (set sched->running = th if sched->running == NULL)
    thread_sched_to_ready_common(sched, th, false, false);

    if (sched->running == th) {
        thread_sched_add_running_thread(sched, th);
    }

    // TODO: check SNT number
    thread_sched_wait_running_turn(sched, th, true);
}

// Luke:
//   This is called by ubf functions in thread.c, BLOCKING_REGION, AFTER io is performed
//
// waiting -> ready -> running
//
// `th` had been waiting by `thread_sched_to_waiting()`
// and run a dedicated task (like waitpid and so on).
// After the dedicated task, this function is called
// to join a normal thread-scheduling.
static void
thread_sched_to_running(struct rb_thread_sched *sched, rb_thread_t *th)
{
    thread_sched_lock(sched, th);
    {
        thread_sched_to_running_common(sched, th);
    }
    thread_sched_unlock(sched, th);
}

// Preconditions:
// sched->running != NULL and sched->running == th (therefore th != NULL)
// sched->running->nt != NULL
// sched is locked
//
// resume a next thread in the thread ready queue.
//
// deque next running thread from the ready thread queue and
// resume this thread if available.
//
// If the next thread has a dedicated native thread, simply signal to resume.
// Otherwise, make the ractor ready and other nt will run the ractor and the thread.
static void
thread_sched_wakeup_next_thread(struct rb_thread_sched *sched, rb_thread_t *th, rb_thread_t *next_th, bool given_next_th, bool will_switch)
{
    ASSERT_thread_sched_locked(sched, th);

    VM_ASSERT(sched->running);
    VM_ASSERT(sched->running == th);
    VM_ASSERT(sched->running->nt != NULL);

    // grab the next thread of this ractor and set sched->running to it
    // even if NULL
    if (!given_next_th) {
        next_th = thread_sched_deq(sched); // NOTE: Can be NULL
    }
    debug_threads(stderr, "thread_sched_wakeup_next_thread will_switch:%d nt:%d th:%d next_th:%d\n",
        will_switch, th->nt->serial, th->serial, next_th ? next_th->serial : -1
    );

    RUBY_DEBUG_LOG("next_th:%u", rb_th_serial(next_th));
    VM_ASSERT(th != next_th);

    thread_sched_set_running(sched, next_th); // just sets sched->running = next_th
    thread_sched_wakeup_running_thread(sched, th, next_th, will_switch);

    // Luke: isn't this always the case?
    if (th != next_th) {
        thread_sched_del_running_thread(sched, th);
    } else {
        VM_ASSERT(0); // Luke: does this happen?
    }
}

// running -> waiting
//
// to_dead: false
//   th will run dedicated task.
//   run another ready thread.
// to_dead: true
//   th will be dead.
//   run another ready thread.
static int
thread_sched_to_waiting_common0(struct rb_thread_sched *sched, rb_thread_t *th, bool to_dead, bool force)
{
    ASSERT_thread_sched_locked(sched, th);
    RUBY_DEBUG_LOG("%sth:%u", to_dead ? "to_dead " : "", rb_th_serial(th));
    bool can_switch = to_dead ? !th_has_dedicated_nt(th) : false;
    // Luke: I think this is needed!
    /*if (can_switch && th->ractor != GET_RACTOR()) {*/
        /*can_switch = false;*/
    /*}*/

    debug_threads(stderr, "thread_sched_to_waiting_common0 th:%d can_switch:%d\n", th->serial, can_switch);
    rb_thread_t *next_th = thread_sched_deq(sched); // NOTE: Can be NULL
    if (next_th || force) {
        RB_INTERNAL_THREAD_HOOK(RUBY_INTERNAL_THREAD_EVENT_SUSPENDED, th);
        if (!to_dead) {
            // becomes dedicated so that runtime can create more SNTs if needed
            native_thread_dedicated_inc(th->vm, th->ractor, th->nt);
        }
        thread_sched_wakeup_next_thread(sched, th, next_th, true, can_switch);
        return 1;
    } else {
        return 0;
    }
}

// running -> dead (locked)
static void
thread_sched_to_dead_common(struct rb_thread_sched *sched, rb_thread_t *th)
{
    RUBY_DEBUG_LOG("dedicated:%d", th->nt->dedicated);
    debug_threads(stderr, "thread_sched_to_dead_common: Thread exiting th:%d, dedicated:%d\n", th->serial, th->nt->dedicated);
    thread_sched_to_waiting_common0(sched, th, true, true);
    debug_threads(stderr, "thread_sched_to_dead_common: Thread exited th:%d (EXITED)\n", th->serial);
    RB_INTERNAL_THREAD_HOOK(RUBY_INTERNAL_THREAD_EVENT_EXITED, th);
}

// running -> dead
static void
thread_sched_to_dead(struct rb_thread_sched *sched, rb_thread_t *th)
{
    thread_sched_lock(sched, th);
    {
        thread_sched_to_dead_common(sched, th);
    }
    thread_sched_unlock(sched, th);
}

// running -> waiting (locked)
//
// This thread will run dedicated task (th->nt->dedicated++).
static int
thread_sched_to_waiting_common(struct rb_thread_sched *sched, rb_thread_t *th, bool force)
{
    RUBY_DEBUG_LOG("dedicated:%d", th->nt->dedicated);
    debug_threads(stderr, "thread_sched_to_waiting(common) nt:%d (dedicated:%d) th:%d\n",
        th->nt->serial, th->nt->dedicated, th->serial
    );
    return thread_sched_to_waiting_common0(sched, th, false, force);
}

// running -> waiting
//
// This thread will run a dedicated task.
static int
thread_sched_to_waiting(struct rb_thread_sched *sched, rb_thread_t *th, bool force)
{
    int ret;
    thread_sched_lock(sched, th);
    {
        ret = thread_sched_to_waiting_common(sched, th, force);
    }
    thread_sched_unlock(sched, th);
    return ret;
}

// mini utility func
static void
setup_ubf(rb_thread_t *th, rb_unblock_function_t *func, void *arg)
{
    rb_native_mutex_lock(&th->interrupt_lock);
    {
        th->unblock.func = func;
        th->unblock.arg  = arg;
    }
    rb_native_mutex_unlock(&th->interrupt_lock);
}

static void
ubf_waiting(void *ptr)
{
    rb_thread_t *th = (rb_thread_t *)ptr;
    struct rb_thread_sched *sched = TH_SCHED(th);

    // only once. it is safe because th->interrupt_lock is already acquired.
    th->unblock.func = NULL;
    th->unblock.arg = NULL;

    RUBY_DEBUG_LOG("th:%u", rb_th_serial(th));

    thread_sched_lock(sched, th);
    {
        if (sched->running == th) {
            // not sleeping yet.
        }
        else {
            thread_sched_to_ready_common(sched, th, true, false);
        }
    }
    thread_sched_unlock(sched, th);
}

// running -> waiting
//
// This thread will sleep until other thread wakeup the thread.
static void
thread_sched_to_waiting_until_wakeup(struct rb_thread_sched *sched, rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%u", rb_th_serial(th));
    VM_ASSERT(th != NULL);
    VM_ASSERT(th->nt != NULL);

    RB_VM_SAVE_MACHINE_CONTEXT(th);
    setup_ubf(th, ubf_waiting, (void *)th);

    RB_INTERNAL_THREAD_HOOK(RUBY_INTERNAL_THREAD_EVENT_SUSPENDED, th);
    debug_threads(stderr, "thread_sched_to_waiting_until_wakeup nt:%d th:%d (SUSPENDED)\n",
        th->nt->serial, th->serial
    );

    thread_sched_lock(sched, th);
    {
        if (!RUBY_VM_INTERRUPTED(th->ec)) {
            bool can_direct_transfer = !th_has_dedicated_nt(th);
            debug_threads(stderr, "thread_sched_to_waiting_until_wakeup th:%d !interrupted, can_direct_transfer: %d\n",
                th->serial, can_direct_transfer
            );
            thread_sched_wakeup_next_thread(sched, th, NULL, false, can_direct_transfer);
            thread_sched_wait_running_turn(sched, th, can_direct_transfer);
        }
        else {
            RUBY_DEBUG_LOG("th:%u interrupted", rb_th_serial(th));
            debug_threads(stderr, "thread_sched_to_waiting_until_wakeup th:%d interrupted\n",
                th->nt->serial, th->serial
            );
        }
    }
    thread_sched_unlock(sched, th);

    setup_ubf(th, NULL, NULL);
}

// run another thread in the ready queue.
// continue to run if there are no ready threads.
static bool
thread_sched_yield(struct rb_thread_sched *sched, rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%d sched->readyq_cnt:%d", (int)th->serial, sched->readyq_cnt);
    debug_threads(stderr, "thread_sched_yield yield th:%d\n", th->serial);

    bool yielded = false;
    thread_sched_lock(sched, th);
    {
        if (!ccan_list_empty(&sched->readyq)) {
            RB_INTERNAL_THREAD_HOOK(RUBY_INTERNAL_THREAD_EVENT_SUSPENDED, th);
            RB_VM_SAVE_MACHINE_CONTEXT(th);
            thread_sched_wakeup_next_thread(sched, th, NULL, false, !th_has_dedicated_nt(th));
            bool can_direct_transfer = !th_has_dedicated_nt(th);
            thread_sched_to_ready_common(sched, th, false, can_direct_transfer);
            thread_sched_wait_running_turn(sched, th, can_direct_transfer);
            yielded = true;
        }
        else {
            VM_ASSERT(sched->readyq_cnt == 0);
            debug_threads(stderr, "thread_sched_yield th:%d sched->readyq is empty, can't yield\n", th->serial);
        }
    }
    thread_sched_unlock(sched, th);
    return yielded;
}

void
rb_thread_sched_init(struct rb_thread_sched *sched, bool atfork)
{
    rb_native_mutex_initialize(&sched->lock_);

#if VM_CHECK_MODE
    sched->lock_owner = NULL;
#endif

    ccan_list_head_init(&sched->readyq);
    sched->readyq_cnt = 0;
    ccan_list_node_init(&sched->grq_node);

#if USE_MN_THREADS
    if (!atfork) sched->enable_mn_threads = true; // MN is enabled on Ractors
#endif
}

static void
coroutine_transfer_thread(struct coroutine_context *transfer_from, struct coroutine_context *transfer_to,
                          rb_thread_t *th, struct rb_native_thread *nt, bool to_dead)
{
    debug_threads(stderr, "coroutine_transfer_thread before r:%d nt:%d th:%d\n",
        rb_ractor_id(th->ractor), nt->serial, th->serial
    );
    coroutine_transfer0(transfer_from, transfer_to, to_dead);
}

static void
coroutine_transfer0(struct coroutine_context *transfer_from, struct coroutine_context *transfer_to, bool to_dead)
{
#ifdef RUBY_ASAN_ENABLED
    void **fake_stack = to_dead ? NULL : &transfer_from->fake_stack;
    __sanitizer_start_switch_fiber(fake_stack, transfer_to->stack_base, transfer_to->stack_size);
#endif

    RBIMPL_ATTR_MAYBE_UNUSED()
    struct coroutine_context *returning_from = coroutine_transfer(transfer_from, transfer_to);

    /* if to_dead was passed, the caller is promising that this coroutine is finished and it should
     * never be resumed! */
    VM_ASSERT(!to_dead);
#ifdef RUBY_ASAN_ENABLED
   __sanitizer_finish_switch_fiber(transfer_from->fake_stack,
                                   (const void**)&returning_from->stack_base, &returning_from->stack_size);
#endif

}

// Arguments:
//   nt != NULL
//   next_th != NULL
// Preconditions:
//   sched is locked
//   nt->dedicated == 0
//   next_th->nt == NULL
// run `next_th` on the given native thread
static void
thread_sched_switch0(struct coroutine_context *current_cont, rb_thread_t *next_th, struct rb_native_thread *nt, bool to_dead)
{
    VM_ASSERT(!nt->dedicated);
    VM_ASSERT(!nt->is_permanent_dnt);
    VM_ASSERT(next_th->nt == NULL);

    RUBY_DEBUG_LOG("next_th:%u", rb_th_serial(next_th));

    ruby_thread_set_native(next_th);
    native_thread_assign(nt, next_th);
    next_th->running_time_us = 0;

    // will pick up where it left off
    coroutine_transfer_thread(current_cont, next_th->sched.context, next_th, nt, to_dead);
}

// Arguments:
//   cth != NULL
//   next_th != NULL
// Precondition:
//   sched is locked
static void
thread_sched_switch(rb_thread_t *cth, rb_thread_t *next_th)
{
    VM_ASSERT(cth);
    struct rb_native_thread *nt = cth->nt;
    native_thread_assign(NULL, cth);
    RUBY_DEBUG_LOG("th:%u->%u on nt:%d", rb_th_serial(cth), rb_th_serial(next_th), nt->serial);
    debug_threads(stderr, "thread_sched_switch nt:%d from_th:%d, to_th:%d (to_dead:%d)\n",
        nt->serial, cth->serial, next_th->serial, cth->status == THREAD_KILLED
    );
    thread_sched_switch0(cth->sched.context, next_th, nt, cth->status == THREAD_KILLED);
}

#if VM_CHECK_MODE > 0
RBIMPL_ATTR_MAYBE_UNUSED()
static unsigned int
grq_size(rb_vm_t *vm, rb_ractor_t *cr)
{
    ASSERT_ractor_sched_locked(vm, cr);

    rb_ractor_t *r, *prev_r = NULL;
    unsigned int i = 0;

    ccan_list_for_each(&vm->ractor.sched.grq, r, threads.sched.grq_node) {
        i++;

        VM_ASSERT(r != prev_r);
        prev_r = r;
    }
    return i;
}
#endif


// Preconditions:
//   thread sched is locked
//    r->threads.sched->running != NULL (the next waiting thread)
//    r->threads.sched->running->nt == NULL
//
/* add ractor `r` to vm->ractor.sched.grq (global ready queue of ractors) and call
 * cond_signal(vm->ractor.sched.cond). There needs to a be a ready thread on the ractor
 *
 */
static void
ractor_sched_enq(rb_vm_t *vm, rb_ractor_t *r, bool allow_already_in)
{
    VM_ASSERT(r->pub.self != Qnil); // not main ractor
    struct rb_thread_sched *sched = &r->threads.sched;
    rb_ractor_t *cr = NULL; // timer thread can call this function

    VM_ASSERT(sched->running != NULL);
    VM_ASSERT(sched->running->nt == NULL);

    debug_threads(stderr, "ractor_sched_enq r:%d before lock\n", rb_ractor_id(r));
    ractor_sched_lock(vm, cr);
    {
        debug_threads(stderr, "ractor_sched_enq r:%d after lock\n", rb_ractor_id(r));
        ractor_sched_dump(vm);
        struct ccan_list_node *node = &r->threads.sched.grq_node;
        if (allow_already_in && node->next != node && node->prev != node) {
            debug_threads(stderr, "ractor_sched_enq r:%d, already in queue! Returning.\n",
                rb_ractor_id(r)
            );
            ractor_sched_unlock(vm, cr);
            return;
        }
/*#if 1*/
        /*// check if grq contains r*/
        /*rb_ractor_t *tr;*/
        /*ccan_list_for_each(&vm->ractor.sched.grq, tr, threads.sched.grq_node) {*/
            /*if (r == tr) {*/
                /*debug_threads(stderr, "ractor_sched_enq r:%d, already in queue! ERROR.\n", rb_ractor_id(r));*/
                /*VM_ASSERT(0);*/
            /*}*/
        /*}*/
/*#endif*/

        ccan_list_add_tail(&vm->ractor.sched.grq, &sched->grq_node);
        vm->ractor.sched.grq_cnt++;
        #if VM_CHECK_MODE
        debug_threads(stderr, "grq_size:%d, grq_cnt:%d\n",
            grq_size(vm, cr), vm->ractor.sched.grq_cnt
        );
        #endif
        VM_ASSERT(grq_size(vm, cr) == vm->ractor.sched.grq_cnt);

        RUBY_DEBUG_LOG("r:%u th:%u grq_cnt:%u", rb_ractor_id(r), rb_th_serial(sched->running), vm->ractor.sched.grq_cnt);

        debug_threads(stderr, "ractor_sched_enq r:%d th:%d new grq_cnt:%d before signal cond\n",
            rb_ractor_id(r), rb_th_serial(sched->running), vm->ractor.sched.grq_cnt
        );
        rb_native_cond_signal(&vm->ractor.sched.cond);

    }
    ractor_sched_unlock(vm, cr);
    debug_threads(stderr, "ractor_sched_enq r:%d after unlock\n", rb_ractor_id(r));
}


#ifndef SNT_KEEP_SECONDS
#define SNT_KEEP_SECONDS 0
#endif

#ifndef MINIMUM_SNT
// make at least MINIMUM_SNT snts for debug.
#define MINIMUM_SNT 0
#endif

// SNT dequeues a ractor (blocking operation)
static rb_ractor_t *
ractor_sched_deq(rb_vm_t *vm, rb_ractor_t *cr, struct rb_native_thread *nt)
{
    rb_ractor_t *r;
    VM_ASSERT(!nt->dedicated);

    ractor_sched_lock(vm, cr);
    {
        RUBY_DEBUG_LOG("empty? %d", ccan_list_empty(&vm->ractor.sched.grq));

        VM_ASSERT(rb_current_execution_context(false) == NULL);
        VM_ASSERT(grq_size(vm, cr) == vm->ractor.sched.grq_cnt);

        while ((r = ccan_list_pop(&vm->ractor.sched.grq, rb_ractor_t, threads.sched.grq_node)) == NULL) {
            RUBY_DEBUG_LOG("wait grq_cnt:%d", (int)vm->ractor.sched.grq_cnt);

#if SNT_KEEP_SECONDS > 0
            rb_hrtime_t abs = rb_hrtime_add(rb_hrtime_now(), RB_HRTIME_PER_SEC * SNT_KEEP_SECONDS);
            if (native_cond_timedwait(&vm->ractor.sched.cond, &vm->ractor.sched.lock, &abs) == ETIMEDOUT) {
                RUBY_DEBUG_LOG("timeout, grq_cnt:%d", (int)vm->ractor.sched.grq_cnt);
                VM_ASSERT(r == NULL);
                vm->ractor.sched.snt_cnt--;
                vm->ractor.sched.running_cnt--;
                break;
            }
            else {
                RUBY_DEBUG_LOG("wakeup grq_cnt:%d", (int)vm->ractor.sched.grq_cnt);
            }
#else
            // Luke: takes this branch
            debug_threads(stderr, "ractor_sched_deq cond_wait before, snt:%d grq:%d\n",
                nt->serial, vm->ractor.sched.grq_cnt
            );

            ractor_sched_dump(vm);

            native_thread_set_blocked(nt, NT_BLOCKED_RACTOR_SCHED_DEQ);
            ractor_sched_set_unlocked(vm, cr);
            rb_native_cond_wait(&vm->ractor.sched.cond, &vm->ractor.sched.lock);
            ractor_sched_set_locked(vm, cr);
            native_thread_set_unblocked(nt);

            debug_threads(stderr, "ractor_sched_deq cond_wait after, snt:%d grq:%d\n",
                nt->serial, vm->ractor.sched.grq_cnt - 1
            );

            ractor_sched_dump(vm);

            RUBY_DEBUG_LOG("wakeup grq_cnt:%d", (int)vm->ractor.sched.grq_cnt);
#endif
        }

        VM_ASSERT(rb_current_execution_context(false) == NULL);

        if (r) {
            VM_ASSERT(vm->ractor.sched.grq_cnt > 0);
            vm->ractor.sched.grq_cnt--;
            ccan_list_node_init(&r->threads.sched.grq_node);
            RUBY_DEBUG_LOG("r:%d grq_cnt:%u", (int)rb_ractor_id(r), vm->ractor.sched.grq_cnt);
        }
        else {
            VM_ASSERT(SNT_KEEP_SECONDS > 0);
            // timeout
        }
    }
    debug_threads(stderr, "ractor_sched_deq before return, snt:%d r:%d\n", nt->serial, rb_ractor_id(r));
    ractor_sched_unlock(vm, cr);

    return r;
}

void rb_ractor_lock_self(rb_ractor_t *r);
void rb_ractor_unlock_self(rb_ractor_t *r);

static bool timer_thread_register_waiting(rb_thread_t *th, int fd, enum thread_sched_waiting_flag flags, rb_hrtime_t *rel);
static bool timer_thread_cancel_waiting(rb_thread_t *th);

bool
rb_ractor_sched_sleep(rb_execution_context_t *ec, rb_ractor_t *cr, rb_unblock_function_t *ubf, unsigned long timeout_ms)
{
    // ractor lock of cr is acquired
    // r is sleeping status
    rb_thread_t * volatile th = rb_ec_thread_ptr(ec);
    struct rb_thread_sched *sched = TH_SCHED(th);
    cr->sync.wait.waiting_thread = th; // TODO: multi-thread
    rb_hrtime_t future;
    bool timer_th_registered = false;
    bool timer_th_need_cancel = false;
    if (timeout_ms > 0) {
        rb_hrtime_t future = rb_msec2hrtime(timeout_ms);
        // Luke: TODO: cancel this timer thread waiting during ubf
        timer_thread_register_waiting(th, -1, thread_sched_waiting_timeout, &future);
        timer_th_registered = true;
    }

    setup_ubf(th, ubf, (void *)cr);

    thread_sched_lock(sched, th);
    {
        rb_ractor_unlock_self(cr);
        {
            if (RUBY_VM_INTERRUPTED(th->ec)) {
                RUBY_DEBUG_LOG("interrupted");
                if (timer_th_registered) timer_th_need_cancel = true;
            }
            else if (cr->sync.wait.wakeup_status != wakeup_none) {
                RUBY_DEBUG_LOG("awaken:%d", (int)cr->sync.wait.wakeup_status);
            }
            else {
                // sleep
                RB_VM_SAVE_MACHINE_CONTEXT(th);
                th->status = THREAD_STOPPED_FOREVER;

                RB_INTERNAL_THREAD_HOOK(RUBY_INTERNAL_THREAD_EVENT_SUSPENDED, th);

                bool can_direct_transfer = !th_has_dedicated_nt(th);
                debug_threads(stderr, "rb_ractor_sched_sleep r:%d nt:%d th:%d (SUSPENDED timeout:%lu) - can_direct_transfer:%d\n",
                    rb_ractor_id(th->ractor), th->nt->serial, th->serial, timeout_ms, can_direct_transfer
                );
                thread_sched_wakeup_next_thread(sched, th, NULL, false, can_direct_transfer);
                thread_sched_wait_running_turn(sched, th, can_direct_transfer);
                // TODO: what if not timed out, who wakes him up? Right now it's someone but I'm not sure who
                debug_threads(stderr, "rb_ractor_sched_sleep after r:%d nt:%d th:%d\n",
                    rb_ractor_id(th->ractor), th->nt->serial, th->serial
                );
                th->status = THREAD_RUNNABLE;
                // wakeup
            }
        }
    }
    thread_sched_unlock(sched, th);

    if (timer_th_need_cancel) {
        timer_thread_cancel_waiting(th);
    }

    setup_ubf(th, NULL, NULL);

    rb_ractor_lock_self(cr);
    cr->sync.wait.waiting_thread = NULL; // TODO: multiple threads can be waiting threads
}

void
rb_ractor_sched_wakeup(rb_ractor_t *r)
{
    rb_thread_t *r_th = r->sync.wait.waiting_thread;
    // ractor lock of r is acquired
    struct rb_thread_sched *sched = TH_SCHED(r_th);

    VM_ASSERT(r->sync.wait.wakeup_status != 0);
    VM_ASSERT(r_th->ractor == r);

    thread_sched_lock(sched, r_th);
    {
        if (r_th->status == THREAD_STOPPED_FOREVER) {
            debug_threads(stderr, "rb_ractor_sched_wakeup r:%d th:%d\n",
                rb_ractor_id(r_th->ractor), r_th->serial
            );
            thread_sched_to_ready_common(sched, r_th, true, false);
        }
    }
    thread_sched_unlock(sched, r_th);
}

static bool
ractor_sched_barrier_completed_p(rb_vm_t *vm)
{
    RUBY_DEBUG_LOG("run:%u wait:%u", vm->ractor.sched.running_cnt, vm->ractor.sched.barrier_waiting_cnt);
    debug_threads(stderr, "barrier_completed_p check: running_cnt:%d, waiting_cnt:%d\n",
        vm->ractor.sched.running_cnt, vm->ractor.sched.barrier_waiting_cnt
    );
    VM_ASSERT(vm->ractor.sched.running_cnt - 1 >= vm->ractor.sched.barrier_waiting_cnt);
    return (vm->ractor.sched.running_cnt - vm->ractor.sched.barrier_waiting_cnt) == 1;
}

void
rb_ractor_sched_barrier_start(rb_vm_t *vm, rb_ractor_t *cr)
{
    VM_ASSERT(cr == GET_RACTOR());
    VM_ASSERT(vm->ractor.sync.lock_owner == cr); // VM is locked
    VM_ASSERT(!vm->ractor.sched.barrier_waiting);
    VM_ASSERT(vm->ractor.sched.barrier_waiting_cnt == 0);

    RUBY_DEBUG_LOG("start serial:%u", vm->ractor.sched.barrier_serial);
    debug_threads(stderr, "ractor barrier start r:%d, serial:%u\n",
        rb_ractor_id(cr), vm->ractor.sched.barrier_serial
    );

    unsigned int lock_rec;

    ractor_sched_lock(vm, cr);
    {
        vm->ractor.sched.barrier_waiting = true;

        // release VM lock
        lock_rec = vm->ractor.sync.lock_rec;
        vm->ractor.sync.lock_rec = 0;
        vm->ractor.sync.lock_owner = NULL;
        rb_native_mutex_unlock(&vm->ractor.sync.lock);
        {
            // interrupts all running threads
            rb_thread_t *ith;
            ccan_list_for_each(&vm->ractor.sched.running_threads, ith, sched.node.running_threads) {
                if (ith->ractor != cr) {
                    RUBY_DEBUG_LOG("barrier int:%u", rb_th_serial(ith));
                    debug_threads(stderr, "ractor barrier send barrier interrupt to r:%d\n",
                        rb_ractor_id(ith->ractor)
                    );
                    RUBY_VM_SET_VM_BARRIER_INTERRUPT(ith->ec);
                }
            }

            // wait for other ractors
            while (!ractor_sched_barrier_completed_p(vm)) {
                ractor_sched_set_unlocked(vm, cr);
                rb_native_cond_wait(&vm->ractor.sched.barrier_complete_cond, &vm->ractor.sched.lock);
                ractor_sched_set_locked(vm, cr);
            }
        }
    }
    ractor_sched_unlock(vm, cr);

    // acquire VM lock
    rb_native_mutex_lock(&vm->ractor.sync.lock);
    vm->ractor.sync.lock_rec = lock_rec;
    vm->ractor.sync.lock_owner = cr;

    RUBY_DEBUG_LOG("completed seirial:%u", vm->ractor.sched.barrier_serial);
    debug_threads(stderr, "ractor barrier end r:%d, serial:%u\n",
        rb_ractor_id(cr), vm->ractor.sched.barrier_serial
    );

    ractor_sched_lock(vm, cr);
    {
        vm->ractor.sched.barrier_waiting = false;
        vm->ractor.sched.barrier_serial++;
        vm->ractor.sched.barrier_waiting_cnt = 0;
        rb_native_cond_broadcast(&vm->ractor.sched.barrier_release_cond);
    }
    ractor_sched_unlock(vm, cr);

}

static void
ractor_sched_barrier_join_signal_locked(rb_vm_t *vm)
{
    if (ractor_sched_barrier_completed_p(vm)) {
        rb_native_cond_signal(&vm->ractor.sched.barrier_complete_cond);
    }
}

static void
ractor_sched_barrier_join_wait_locked(rb_vm_t *vm, rb_thread_t *th)
{
    VM_ASSERT(vm->ractor.sched.barrier_waiting);

    unsigned int barrier_serial = vm->ractor.sched.barrier_serial;

    while (vm->ractor.sched.barrier_serial == barrier_serial) {
        RUBY_DEBUG_LOG("sleep serial:%u", barrier_serial);
        RB_VM_SAVE_MACHINE_CONTEXT(th);

        rb_ractor_t *cr = th->ractor;
        ractor_sched_set_unlocked(vm, cr);
        rb_native_cond_wait(&vm->ractor.sched.barrier_release_cond, &vm->ractor.sched.lock);
        ractor_sched_set_locked(vm, cr);

        RUBY_DEBUG_LOG("wakeup serial:%u", barrier_serial);
    }
}

void
rb_ractor_sched_barrier_join(rb_vm_t *vm, rb_ractor_t *cr)
{
    VM_ASSERT(cr->threads.sched.running != NULL); // running ractor
    VM_ASSERT(cr == GET_RACTOR());
    VM_ASSERT(vm->ractor.sync.lock_owner == NULL); // VM is locked, but owner == NULL
    VM_ASSERT(vm->ractor.sched.barrier_waiting);  // VM needs barrier sync

#if USE_RUBY_DEBUG_LOG || VM_CHECK_MODE > 0
    unsigned int barrier_serial = vm->ractor.sched.barrier_serial;
#endif

    RUBY_DEBUG_LOG("join");
    debug_threads(stderr, "rb_ractor_sched_barrier_join r:%d, serial:%u\n",
        rb_ractor_id(cr), vm->ractor.sched.barrier_serial
    );

    rb_native_mutex_unlock(&vm->ractor.sync.lock);
    {
        VM_ASSERT(vm->ractor.sched.barrier_waiting);  // VM needs barrier sync
        VM_ASSERT(vm->ractor.sched.barrier_serial == barrier_serial);

        ractor_sched_lock(vm, cr);
        {
            // running_cnt
            vm->ractor.sched.barrier_waiting_cnt++;
            RUBY_DEBUG_LOG("waiting_cnt:%u serial:%u", vm->ractor.sched.barrier_waiting_cnt, barrier_serial);

            ractor_sched_barrier_join_signal_locked(vm);
            ractor_sched_barrier_join_wait_locked(vm, cr->threads.sched.running);
        }
        ractor_sched_unlock(vm, cr);
    }

    rb_native_mutex_lock(&vm->ractor.sync.lock);
    // VM locked here
}

#if 0
// TODO

static void clear_thread_cache_altstack(void);

static void
rb_thread_sched_destroy(struct rb_thread_sched *sched)
{
    /*
     * only called once at VM shutdown (not atfork), another thread
     * may still grab vm->gvl.lock when calling gvl_release at
     * the end of thread_start_func_2
     */
    if (0) {
        rb_native_mutex_destroy(&sched->lock);
    }
    clear_thread_cache_altstack();
}
#endif

#if defined(HAVE_WORKING_FORK)
static void
thread_sched_atfork(struct rb_thread_sched *sched)
{
    current_fork_gen++;
    rb_thread_sched_init(sched, true);
    rb_thread_t *th =  GET_THREAD();
    rb_vm_t *vm = GET_VM();

    if (th_has_dedicated_nt(th)) {
        vm->ractor.sched.snt_cnt = 0;
    }
    else {
        vm->ractor.sched.snt_cnt = 1;
    }
    vm->ractor.sched.running_cnt = 0;

    rb_native_mutex_initialize(&vm->ractor.sched.lock);
    // rb_native_cond_destroy(&vm->ractor.sched.cond);
    rb_native_cond_initialize(&vm->ractor.sched.cond);
    rb_native_cond_initialize(&vm->ractor.sched.barrier_complete_cond);
    rb_native_cond_initialize(&vm->ractor.sched.barrier_release_cond);

    ccan_list_head_init(&vm->ractor.sched.grq);
    ccan_list_head_init(&vm->ractor.sched.timeslice_threads);
    ccan_list_head_init(&vm->ractor.sched.running_threads);

    VM_ASSERT(sched->is_running);
    sched->is_running_timeslice = false;

    if (sched->running != th) {
        thread_sched_to_running(sched, th);
    }
    else {
        thread_sched_setup_running_threads(sched, th->ractor, vm, th, NULL, NULL);
    }

#ifdef RB_THREAD_T_HAS_NATIVE_ID
    if (th->nt) {
        th->nt->tid = get_native_thread_id();
    }
#endif
}

#endif

#ifdef RB_THREAD_LOCAL_SPECIFIER
static RB_THREAD_LOCAL_SPECIFIER rb_thread_t *ruby_native_thread;
#else
static pthread_key_t ruby_native_thread_key;
#endif

static void
null_func(int i)
{
    /* null */
    // This function can be called from signal handler
    // RUBY_DEBUG_LOG("i:%d", i);
}

rb_thread_t *
ruby_thread_from_native(void)
{
#ifdef RB_THREAD_LOCAL_SPECIFIER
    return ruby_native_thread;
#else
    return pthread_getspecific(ruby_native_thread_key);
#endif
}

// Arguments:
//   th != NULL
int
ruby_thread_set_native(rb_thread_t *th)
{
    if (th) {
#ifdef USE_UBF_LIST
        ccan_list_node_init(&th->sched.node.ubf);
#endif
    }

    // setup TLS

    if (th && th->ec) {
        debug_threads(stderr, "ruby_thread_set_native r:%d th:%d->ec\n",
            rb_ractor_id(th->ractor), th->serial
        );
        rb_ractor_set_current_ec(th->ractor, th->ec);
    }
#ifdef RB_THREAD_LOCAL_SPECIFIER
    ruby_native_thread = th;
    return 1;
#else
    return pthread_setspecific(ruby_native_thread_key, th) == 0;
#endif
}

static void native_thread_setup(struct rb_native_thread *nt);
static void native_thread_setup_on_thread(struct rb_native_thread *nt);

/* Init main thread */
void
Init_native_thread(rb_thread_t *main_th)
{
#if defined(HAVE_PTHREAD_CONDATTR_SETCLOCK)
    if (condattr_monotonic) {
        int r = pthread_condattr_init(condattr_monotonic);
        if (r == 0) {
            r = pthread_condattr_setclock(condattr_monotonic, CLOCK_MONOTONIC);
        }
        if (r) condattr_monotonic = NULL;
    }
#endif

#ifndef RB_THREAD_LOCAL_SPECIFIER
    if (pthread_key_create(&ruby_native_thread_key, 0) == EAGAIN) {
        rb_bug("pthread_key_create failed (ruby_native_thread_key)");
    }
    if (pthread_key_create(&ruby_current_ec_key, 0) == EAGAIN) {
        rb_bug("pthread_key_create failed (ruby_current_ec_key)");
    }
#endif
    ruby_posix_signal(SIGVTALRM, null_func);

    // setup vm
    rb_vm_t *vm = main_th->vm;
    rb_native_mutex_initialize(&vm->ractor.sched.lock);
    rb_native_cond_initialize(&vm->ractor.sched.cond);
    rb_native_cond_initialize(&vm->ractor.sched.barrier_complete_cond);
    rb_native_cond_initialize(&vm->ractor.sched.barrier_release_cond);

    ccan_list_head_init(&vm->ractor.sched.grq);
    ccan_list_head_init(&vm->ractor.sched.timeslice_threads);
    ccan_list_head_init(&vm->ractor.sched.running_threads);

    // setup main thread
    main_th->nt->thread_id = pthread_self();
    main_th->nt->serial = 1;
    main_th->serial = 1;
#ifdef RUBY_NT_SERIAL
    ruby_nt_serial = 1;
#endif
    ruby_thread_set_native(main_th);
    native_thread_setup(main_th->nt);
    native_thread_setup_on_thread(main_th->nt);

    TH_SCHED(main_th)->running = main_th;
    main_th->has_dedicated_nt = 1;

    thread_sched_setup_running_threads(TH_SCHED(main_th), main_th->ractor, vm, main_th, NULL, NULL);

    // setup main NT
    main_th->nt->dedicated = 1;
    main_th->nt->is_permanent_dnt = true;
    main_th->nt->vm = vm;

    // setup mn
    vm->ractor.sched.dnt_cnt = 1;
}

extern int ruby_mn_threads_enabled;

void
ruby_mn_threads_params(void)
{
    rb_vm_t *vm = GET_VM();
    rb_ractor_t *main_ractor = GET_RACTOR();

    const char *mn_threads_cstr = getenv("RUBY_MN_THREADS");
    bool enable_mn_threads = false;

    if (USE_MN_THREADS && mn_threads_cstr && (enable_mn_threads = atoi(mn_threads_cstr) > 0)) {
        // enabled
        ruby_mn_threads_enabled = 1;
    }
    main_ractor->threads.sched.enable_mn_threads = enable_mn_threads;

    const char *max_cpu_cstr = getenv("RUBY_MAX_CPU");
    const int default_max_cpu = 8; // TODO: CPU num?
    int max_cpu = default_max_cpu;

    if (USE_MN_THREADS && max_cpu_cstr)  {
        int given_max_cpu = atoi(max_cpu_cstr);
        if (given_max_cpu > 0) {
            max_cpu = given_max_cpu;
        }
    }

    vm->ractor.sched.max_cpu = max_cpu;
}

static void
native_thread_dedicated_inc(rb_vm_t *vm, rb_ractor_t *cr, struct rb_native_thread *nt)
{
    RUBY_DEBUG_LOG("nt:%d %d->%d", nt->serial, nt->dedicated, nt->dedicated + 1);

    if (!nt->is_permanent_dnt) {
        if (nt->dedicated == 0) {
            ractor_sched_lock(vm, cr); // TODO: remove
            {
                vm->ractor.sched.snt_cnt--;
                vm->ractor.sched.dnt_cnt++;
            }
            ractor_sched_unlock(vm, cr);
        }

        // Luke: only current nt touches this field
        nt->dedicated++;
    }
}

static void
native_thread_dedicated_dec(rb_vm_t *vm, rb_ractor_t *cr, struct rb_native_thread *nt)
{
    RUBY_DEBUG_LOG("nt:%d %d->%d", nt->serial, nt->dedicated, nt->dedicated - 1);
    if (!nt->is_permanent_dnt) {
        VM_ASSERT(nt->dedicated > 0);
        nt->dedicated--;

        if (nt->dedicated == 0) {
            ractor_sched_lock(vm, cr); // TODO: remove
            {
                nt->vm->ractor.sched.snt_cnt++;
                nt->vm->ractor.sched.dnt_cnt--;
            }
            ractor_sched_unlock(vm, cr);
        }
    }
}

// th->nt = nt
static void
native_thread_assign(struct rb_native_thread *nt, rb_thread_t *th)
{
#if USE_RUBY_DEBUG_LOG
    if (nt) {
        if (th->nt) {
            RUBY_DEBUG_LOG("th:%d nt:%d->%d", (int)th->serial, (int)th->nt->serial, (int)nt->serial);
            debug_threads(stderr, "native_thread_assign th:%d nt:%d->%d\n",
                th->serial, th->nt->serial, nt->serial
            );
        }
        else {
            RUBY_DEBUG_LOG("th:%d nt:NULL->%d", (int)th->serial, (int)nt->serial);
            debug_threads(stderr, "native_thread_assign th:%d nt:NULL->%d\n",
                th->serial, nt->serial
            );
        }
    }
    else {
        if (th->nt) {
            RUBY_DEBUG_LOG("th:%d nt:%d->NULL", (int)th->serial, (int)th->nt->serial);
            debug_threads(stderr, "native_thread_assign th:%d nt:%d->NULL\n",
                th->serial, th->nt->serial
            );
        }
        else {
            RUBY_DEBUG_LOG("th:%d nt:NULL->NULL", (int)th->serial);
            debug_threads(stderr, "native_thread_assign th:%d nt:NULL->NULL\n",
                th->serial
            );
        }
    }
#endif

    th->nt = nt;
}

static void
native_thread_set_blocked(struct rb_native_thread *nt, enum nt_blocked_reason reason)
{
    VM_ASSERT(nt->blocked_reason == NT_BLOCKED_NONE);
    nt->blocked_reason = reason;
}

static void
native_thread_set_blocked_unchecked(struct rb_native_thread *nt, enum nt_blocked_reason reason)
{
    nt->blocked_reason = reason;
}

static void
native_thread_set_unblocked(struct rb_native_thread *nt)
{
    VM_ASSERT(nt->blocked_reason != NT_BLOCKED_NONE);
    nt->blocked_reason = NT_BLOCKED_NONE;
}

static void
native_thread_destroy(struct rb_native_thread *nt)
{
    if (nt) {
        rb_native_cond_destroy(&nt->cond.readyq);

        if (&nt->cond.readyq != &nt->cond.intr) {
            rb_native_cond_destroy(&nt->cond.intr);
        }

        RB_ALTSTACK_FREE(nt->altstack);
        ruby_xfree(nt->nt_context);
        ruby_xfree(nt);
    }
}

#if defined HAVE_PTHREAD_GETATTR_NP || defined HAVE_PTHREAD_ATTR_GET_NP
#define STACKADDR_AVAILABLE 1
#elif defined HAVE_PTHREAD_GET_STACKADDR_NP && defined HAVE_PTHREAD_GET_STACKSIZE_NP
#define STACKADDR_AVAILABLE 1
#undef MAINSTACKADDR_AVAILABLE
#define MAINSTACKADDR_AVAILABLE 1
void *pthread_get_stackaddr_np(pthread_t);
size_t pthread_get_stacksize_np(pthread_t);
#elif defined HAVE_THR_STKSEGMENT || defined HAVE_PTHREAD_STACKSEG_NP
#define STACKADDR_AVAILABLE 1
#elif defined HAVE_PTHREAD_GETTHRDS_NP
#define STACKADDR_AVAILABLE 1
#elif defined __HAIKU__
#define STACKADDR_AVAILABLE 1
#endif

#ifndef MAINSTACKADDR_AVAILABLE
# ifdef STACKADDR_AVAILABLE
#   define MAINSTACKADDR_AVAILABLE 1
# else
#   define MAINSTACKADDR_AVAILABLE 0
# endif
#endif
#if MAINSTACKADDR_AVAILABLE && !defined(get_main_stack)
# define get_main_stack(addr, size) get_stack(addr, size)
#endif

#ifdef STACKADDR_AVAILABLE
/*
 * Get the initial address and size of current thread's stack
 */
static int
get_stack(void **addr, size_t *size)
{
#define CHECK_ERR(expr)				\
    {int err = (expr); if (err) return err;}
#ifdef HAVE_PTHREAD_GETATTR_NP /* Linux */
    pthread_attr_t attr;
    size_t guard = 0;
    STACK_GROW_DIR_DETECTION;
    CHECK_ERR(pthread_getattr_np(pthread_self(), &attr));
# ifdef HAVE_PTHREAD_ATTR_GETSTACK
    CHECK_ERR(pthread_attr_getstack(&attr, addr, size));
    STACK_DIR_UPPER((void)0, (void)(*addr = (char *)*addr + *size));
# else
    CHECK_ERR(pthread_attr_getstackaddr(&attr, addr));
    CHECK_ERR(pthread_attr_getstacksize(&attr, size));
# endif
# ifdef HAVE_PTHREAD_ATTR_GETGUARDSIZE
    CHECK_ERR(pthread_attr_getguardsize(&attr, &guard));
# else
    guard = getpagesize();
# endif
    *size -= guard;
    pthread_attr_destroy(&attr);
#elif defined HAVE_PTHREAD_ATTR_GET_NP /* FreeBSD, DragonFly BSD, NetBSD */
    pthread_attr_t attr;
    CHECK_ERR(pthread_attr_init(&attr));
    CHECK_ERR(pthread_attr_get_np(pthread_self(), &attr));
# ifdef HAVE_PTHREAD_ATTR_GETSTACK
    CHECK_ERR(pthread_attr_getstack(&attr, addr, size));
# else
    CHECK_ERR(pthread_attr_getstackaddr(&attr, addr));
    CHECK_ERR(pthread_attr_getstacksize(&attr, size));
# endif
    STACK_DIR_UPPER((void)0, (void)(*addr = (char *)*addr + *size));
    pthread_attr_destroy(&attr);
#elif (defined HAVE_PTHREAD_GET_STACKADDR_NP && defined HAVE_PTHREAD_GET_STACKSIZE_NP) /* MacOS X */
    pthread_t th = pthread_self();
    *addr = pthread_get_stackaddr_np(th);
    *size = pthread_get_stacksize_np(th);
#elif defined HAVE_THR_STKSEGMENT || defined HAVE_PTHREAD_STACKSEG_NP
    stack_t stk;
# if defined HAVE_THR_STKSEGMENT /* Solaris */
    CHECK_ERR(thr_stksegment(&stk));
# else /* OpenBSD */
    CHECK_ERR(pthread_stackseg_np(pthread_self(), &stk));
# endif
    *addr = stk.ss_sp;
    *size = stk.ss_size;
#elif defined HAVE_PTHREAD_GETTHRDS_NP /* AIX */
    pthread_t th = pthread_self();
    struct __pthrdsinfo thinfo;
    char reg[256];
    int regsiz=sizeof(reg);
    CHECK_ERR(pthread_getthrds_np(&th, PTHRDSINFO_QUERY_ALL,
                                   &thinfo, sizeof(thinfo),
                                   &reg, &regsiz));
    *addr = thinfo.__pi_stackaddr;
    /* Must not use thinfo.__pi_stacksize for size.
       It is around 3KB smaller than the correct size
       calculated by thinfo.__pi_stackend - thinfo.__pi_stackaddr. */
    *size = thinfo.__pi_stackend - thinfo.__pi_stackaddr;
    STACK_DIR_UPPER((void)0, (void)(*addr = (char *)*addr + *size));
#elif defined __HAIKU__
    thread_info info;
    STACK_GROW_DIR_DETECTION;
    CHECK_ERR(get_thread_info(find_thread(NULL), &info));
    *addr = info.stack_base;
    *size = (uintptr_t)info.stack_end - (uintptr_t)info.stack_base;
    STACK_DIR_UPPER((void)0, (void)(*addr = (char *)*addr + *size));
#else
#error STACKADDR_AVAILABLE is defined but not implemented.
#endif
    return 0;
#undef CHECK_ERR
}
#endif

static struct {
    rb_nativethread_id_t id;
    size_t stack_maxsize;
    VALUE *stack_start;
} native_main_thread;

#ifdef STACK_END_ADDRESS
extern void *STACK_END_ADDRESS;
#endif

enum {
    RUBY_STACK_SPACE_LIMIT = 1024 * 1024, /* 1024KB */
    RUBY_STACK_SPACE_RATIO = 5
};

static size_t
space_size(size_t stack_size)
{
    size_t space_size = stack_size / RUBY_STACK_SPACE_RATIO;
    if (space_size > RUBY_STACK_SPACE_LIMIT) {
        return RUBY_STACK_SPACE_LIMIT;
    }
    else {
        return space_size;
    }
}

static void
native_thread_init_main_thread_stack(void *addr)
{
    native_main_thread.id = pthread_self();
#ifdef RUBY_ASAN_ENABLED
    addr = asan_get_real_stack_addr((void *)addr);
#endif

#if MAINSTACKADDR_AVAILABLE
    if (native_main_thread.stack_maxsize) return;
    {
        void* stackaddr;
        size_t size;
        if (get_main_stack(&stackaddr, &size) == 0) {
            native_main_thread.stack_maxsize = size;
            native_main_thread.stack_start = stackaddr;
            goto bound_check;
        }
    }
#endif
#ifdef STACK_END_ADDRESS
    native_main_thread.stack_start = STACK_END_ADDRESS;
#else
    if (!native_main_thread.stack_start ||
        STACK_UPPER((VALUE *)(void *)&addr,
                    native_main_thread.stack_start > (VALUE *)addr,
                    native_main_thread.stack_start < (VALUE *)addr)) {
        native_main_thread.stack_start = (VALUE *)addr;
    }
#endif
    {
#if defined(HAVE_GETRLIMIT)
#if defined(PTHREAD_STACK_DEFAULT)
# if PTHREAD_STACK_DEFAULT < RUBY_STACK_SPACE*5
#  error "PTHREAD_STACK_DEFAULT is too small"
# endif
        size_t size = PTHREAD_STACK_DEFAULT;
#else
        size_t size = RUBY_VM_THREAD_VM_STACK_SIZE;
#endif
        size_t space;
        int pagesize = getpagesize();
        struct rlimit rlim;
        STACK_GROW_DIR_DETECTION;
        if (getrlimit(RLIMIT_STACK, &rlim) == 0) {
            size = (size_t)rlim.rlim_cur;
        }
        addr = native_main_thread.stack_start;
        if (IS_STACK_DIR_UPPER()) {
            space = ((size_t)((char *)addr + size) / pagesize) * pagesize - (size_t)addr;
        }
        else {
            space = (size_t)addr - ((size_t)((char *)addr - size) / pagesize + 1) * pagesize;
        }
        native_main_thread.stack_maxsize = space;
#endif
    }

#if MAINSTACKADDR_AVAILABLE
  bound_check:
#endif
    /* If addr is out of range of main-thread stack range estimation,  */
    /* it should be on co-routine (alternative stack). [Feature #2294] */
    {
        void *start, *end;
        STACK_GROW_DIR_DETECTION;

        if (IS_STACK_DIR_UPPER()) {
            start = native_main_thread.stack_start;
            end = (char *)native_main_thread.stack_start + native_main_thread.stack_maxsize;
        }
        else {
            start = (char *)native_main_thread.stack_start - native_main_thread.stack_maxsize;
            end = native_main_thread.stack_start;
        }

        if ((void *)addr < start || (void *)addr > end) {
            /* out of range */
            native_main_thread.stack_start = (VALUE *)addr;
            native_main_thread.stack_maxsize = 0; /* unknown */
        }
    }
}

#define CHECK_ERR(expr) \
    {int err = (expr); if (err) {rb_bug_errno(#expr, err);}}

static int
native_thread_init_stack(rb_thread_t *th, void *local_in_parent_frame)
{
    rb_nativethread_id_t curr = pthread_self();
#ifdef RUBY_ASAN_ENABLED
    local_in_parent_frame = asan_get_real_stack_addr(local_in_parent_frame);
    th->ec->machine.asan_fake_stack_handle = asan_get_thread_fake_stack_handle();
#endif

    if (!native_main_thread.id) {
        /* This thread is the first thread, must be the main thread -
         * configure the native_main_thread object */
        native_thread_init_main_thread_stack(local_in_parent_frame);
    }

    if (pthread_equal(curr, native_main_thread.id)) {
        th->ec->machine.stack_start = native_main_thread.stack_start;
        th->ec->machine.stack_maxsize = native_main_thread.stack_maxsize;
    }
    else {
#ifdef STACKADDR_AVAILABLE
        if (th_has_dedicated_nt(th)) {
            void *start;
            size_t size;

            if (get_stack(&start, &size) == 0) {
                uintptr_t diff = (uintptr_t)start - (uintptr_t)local_in_parent_frame;
                th->ec->machine.stack_start = local_in_parent_frame;
                th->ec->machine.stack_maxsize = size - diff;
            }
        }
#else
        rb_raise(rb_eNotImpError, "ruby engine can initialize only in the main thread");
#endif
    }

    return 0;
}

struct nt_param {
    rb_vm_t *vm;
    struct rb_native_thread *nt;
};

static void *
nt_start(void *ptr);

static int
native_thread_create0(struct rb_native_thread *nt)
{
    int err = 0;
    pthread_attr_t attr;

    const size_t stack_size = nt->vm->default_params.thread_machine_stack_size;
    const size_t space = space_size(stack_size);

    nt->machine_stack_maxsize = stack_size - space;

#ifdef USE_SIGALTSTACK
    nt->altstack = rb_allocate_sigaltstack();
#endif

    CHECK_ERR(pthread_attr_init(&attr));

# ifdef PTHREAD_STACK_MIN
    RUBY_DEBUG_LOG("stack size: %lu", (unsigned long)stack_size);
    CHECK_ERR(pthread_attr_setstacksize(&attr, stack_size));
# endif

# ifdef HAVE_PTHREAD_ATTR_SETINHERITSCHED
    CHECK_ERR(pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED));
# endif
    CHECK_ERR(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));

    err = pthread_create(&nt->thread_id, &attr, nt_start, nt);

    if (err) {
        fprintf(stderr, "pthread_create err: %d\n", err);
    }

    RUBY_DEBUG_LOG("nt:%d err:%d", (int)nt->serial, err);

    CHECK_ERR(pthread_attr_destroy(&attr));

    return err;
}

static void
native_thread_setup(struct rb_native_thread *nt)
{
    // init cond
    rb_native_cond_initialize(&nt->cond.readyq);

    if (&nt->cond.readyq != &nt->cond.intr) {
        rb_native_cond_initialize(&nt->cond.intr);
    }
}

static void
native_thread_setup_on_thread(struct rb_native_thread *nt)
{
    // init tid
#if defined(RB_THREAD_T_HAS_NATIVE_ID) || defined(__APPLE__)
    nt->tid = get_native_thread_id();
#endif

    // init signal handler
    RB_ALTSTACK_INIT(nt->altstack, nt->altstack);
}

static struct rb_native_thread *
native_thread_alloc(void)
{
    struct rb_native_thread *nt = ZALLOC(struct rb_native_thread);
    VM_ASSERT(nt);
    native_thread_setup(nt);

#if USE_MN_THREADS
    // TODO: look into if this is needed if it's a DNT
    nt->nt_context = ruby_xmalloc(sizeof(struct coroutine_context));
#endif

#ifdef RUBY_NT_SERIAL_MINE
    static rb_atomic_t nt_serial = 2;
    nt->serial = RUBY_ATOMIC_FETCH_ADD(nt_serial, 1);
#endif
    return nt;
}

static int
native_thread_create_dedicated(rb_thread_t *th)
{
    th->nt = native_thread_alloc();
    th->nt->vm = th->vm;
    th->nt->running_thread = th;
    th->nt->dedicated = 1;
    th->nt->is_permanent_dnt = true;

    // vm stack
    size_t vm_stack_word_size = th->vm->default_params.thread_vm_stack_size / sizeof(VALUE);
    void *vm_stack = ruby_xmalloc(vm_stack_word_size * sizeof(VALUE));
    th->sched.malloc_stack = true;
    rb_ec_initialize_vm_stack(th->ec, vm_stack, vm_stack_word_size);
    th->sched.context_stack = vm_stack;


    int err = native_thread_create0(th->nt);
    if (!err) {
        // setup
        thread_sched_to_ready(TH_SCHED(th), th);
    }
    return err;
}

static void
call_thread_start_func_2(rb_thread_t *th)
{
    /* Capture the address of a local in this stack frame to mark the beginning of the
       machine stack for this thread. This is required even if we can tell the real
       stack beginning from the pthread API in native_thread_init_stack, because
       glibc stores some of its own data on the stack before calling into user code
       on a new thread, and replacing that data on fiber-switch would break it (see
       bug #13887) */
    VALUE stack_start = 0;
    VALUE *stack_start_addr = asan_get_real_stack_addr(&stack_start);

    native_thread_init_stack(th, stack_start_addr);
    thread_start_func_2(th, th->ec->machine.stack_start);
}

static void *
nt_start(void *ptr)
{
    struct rb_native_thread *nt = (struct rb_native_thread *)ptr;
    rb_vm_t *vm = nt->vm;

    native_thread_setup_on_thread(nt);

#ifdef RUBY_NT_SERIAL
    ruby_nt_serial = nt->serial;
#endif

    RUBY_DEBUG_LOG("nt:%u", nt->serial);

    if (!nt->dedicated) {
        coroutine_initialize_main(nt->nt_context);
    }

    while (1) {
        if (nt->dedicated) {
            // wait running turn
            rb_thread_t *th = nt->running_thread;
            VM_ASSERT(th);
            struct rb_thread_sched *sched = TH_SCHED(th);

            RUBY_DEBUG_LOG("on dedicated th:%u", rb_th_serial(th));
            debug_threads(stderr, "nt_start nt->dedicated nt:%d, running_thread:%d\n", nt->serial, th->serial);
            ruby_thread_set_native(th);

            thread_sched_lock(sched, th);
            {
                if (sched->running == th) {
                    thread_sched_add_running_thread(sched, th);
                }
                thread_sched_wait_running_turn(sched, th, false);
            }
            thread_sched_unlock(sched, th);

            // start threads
            debug_threads(stderr, "nt_start nt->dedicated nt:%d, running_thread:%d, call_thread_start_func_2\n", nt->serial, th->serial);
            VM_ASSERT(th->running_time_us == 0);
            call_thread_start_func_2(th); // sched->running == th
            break; // TODO: allow to change to the SNT
        }
        else {
            RUBY_DEBUG_LOG("check next");
            // thread is parked, waiting for ractor
            debug_threads(stderr, "snt:%d parked looking for ractors\n", nt->serial);
            rb_ractor_t *r = ractor_sched_deq(vm, NULL, nt);

            if (r) {
                struct rb_thread_sched *sched = &r->threads.sched;

                thread_sched_lock(sched, NULL);
                {
                    rb_thread_t *next_th = sched->running;

                    if (next_th && next_th->nt == NULL) {
                        RUBY_DEBUG_LOG("nt:%d next_th:%d", (int)nt->serial, (int)next_th->serial);
                        debug_threads(stderr, "snt:%d picked up r:%d, will run th:%d (%s)\n",
                            nt->serial, rb_ractor_id(r), next_th->serial, thread_status_name(next_th, true)
                        );
                        // will pick up where the thread left off
                        thread_sched_switch0(nt->nt_context, next_th, nt, false);
                        debug_threads(stderr, "snt:%d finished with r:%d th:%d\n",
                            nt->serial, rb_ractor_id(r), next_th->serial
                        );
                    }
                    else {
                        debug_threads(stderr, "snt:%d picked up r:%d, but got NO next_th\n", nt->serial, rb_ractor_id(r));
                        RUBY_DEBUG_LOG("no schedulable threads -- next_th:%p", next_th);
                    }
                }
                thread_sched_unlock(sched, NULL);
            }
            else {
                debug_threads(stderr, "ractor_sched_deq timeout??? nt: %d\n", nt->serial);
                // timeout -> deleted.
                break;
            }

            if (nt->dedicated) {
                debug_threads(stderr, "SNT becomes DNT:%d\n", nt->serial);
                // SNT becomes DNT while running
                break;
            }
        }
    }

    // Luke: TODO: look into this
    /*rb_native_mutex_lock(&vm->ractor.sched.lock);*/
    /*{*/
        /*if (nt->dedicated) {*/
            /*VM_ASSERT(vm->ractor.sched.dnt_cnt > 0);*/
            /*debug_threads(stderr, "nt_start return for snt->dnt:%d (snt_cnt:%d dnt_cnt:%d)\n",*/
                /*nt->serial, vm->ractor.sched.snt_cnt, vm->ractor.sched.dnt_cnt-1*/
            /*);*/
            /*vm->ractor.sched.dnt_cnt--;*/
            /*nt->is_permanent_dnt = false;*/
            /*nt->dedicated = 0;*/
        /*} else {*/
            /*VM_ASSERT(vm->ractor.sched.snt_cnt > 0);*/
            /*debug_threads(stderr, "nt_start return for snt:%d (snt_cnt:%d dnt_cnt:%d)\n",*/
                /*nt->serial, vm->ractor.sched.snt_cnt-1, vm->ractor.sched.dnt_cnt*/
            /*);*/
            /*vm->ractor.sched.snt_cnt--;*/
        /*}*/
    /*}*/
    /*rb_native_mutex_unlock(&vm->ractor.sched.lock);*/
    return NULL;
}

static int native_thread_create_shared(rb_thread_t *th);

#if USE_MN_THREADS
static void nt_free_stack(void *mstack);
#endif

void
rb_threadptr_remove(rb_thread_t *th)
{
#if USE_MN_THREADS
    if (th->sched.malloc_stack) {
        // dedicated
        return;
    }
    else {
        rb_vm_t *vm = th->vm;
        th->sched.finished = false;

        RB_VM_LOCK_ENTER();
        {
            ccan_list_add(&vm->ractor.sched.zombie_threads, &th->sched.node.zombie_threads);
        }
        RB_VM_LOCK_LEAVE();
    }
#endif
}

void
rb_threadptr_sched_free(rb_thread_t *th)
{
#if USE_MN_THREADS
    if (th->sched.malloc_stack) {
        // has dedicated
        ruby_xfree(th->sched.context_stack);
        native_thread_destroy(th->nt);
    }
    else {
        nt_free_stack(th->sched.context_stack);
        // TODO: how to free nt and nt->altstack?
    }

    ruby_xfree(th->sched.context);
    th->sched.context = NULL;
    // VM_ASSERT(th->sched.context == NULL);
#else
    ruby_xfree(th->sched.context_stack);
    native_thread_destroy(th->nt);
#endif

    th->nt = NULL;
}

void
rb_thread_sched_mark_zombies(rb_vm_t *vm)
{
    if (!ccan_list_empty(&vm->ractor.sched.zombie_threads)) {
        rb_thread_t *zombie_th, *next_zombie_th;
        ccan_list_for_each_safe(&vm->ractor.sched.zombie_threads, zombie_th, next_zombie_th, sched.node.zombie_threads) {
            if (zombie_th->sched.finished) {
                ccan_list_del_init(&zombie_th->sched.node.zombie_threads);
            }
            else {
                rb_gc_mark(zombie_th->self);
            }
        }
    }
}

static int
native_thread_create(rb_thread_t *th)
{
    VM_ASSERT(th->nt == 0);
    RUBY_DEBUG_LOG("th:%d has_dnt:%d", th->serial, th->has_dedicated_nt);
    RB_INTERNAL_THREAD_HOOK(RUBY_INTERNAL_THREAD_EVENT_STARTED, th);

    if (!th->ractor->threads.sched.enable_mn_threads) {
        th->has_dedicated_nt = 1;
    }

    /* Dedicated threads are created in main ractor (if RUBY_MN_THREADS != 1) */
    if (th->has_dedicated_nt) {
        debug_threads(stderr, "creating dedicated thread\n"); // Luke: This somehow causes timer thread kevent to return
        return native_thread_create_dedicated(th);
    }
    else {
        debug_threads(stderr, "might create shared thread\n");
        return native_thread_create_shared(th);
    }
}

#if USE_NATIVE_THREAD_PRIORITY

static void
native_thread_apply_priority(rb_thread_t *th)
{
#if defined(_POSIX_PRIORITY_SCHEDULING) && (_POSIX_PRIORITY_SCHEDULING > 0)
    struct sched_param sp;
    int policy;
    int priority = 0 - th->priority;
    int max, min;
    pthread_getschedparam(th->nt->thread_id, &policy, &sp);
    max = sched_get_priority_max(policy);
    min = sched_get_priority_min(policy);

    if (min > priority) {
        priority = min;
    }
    else if (max < priority) {
        priority = max;
    }

    sp.sched_priority = priority;
    pthread_setschedparam(th->nt->thread_id, policy, &sp);
#else
    /* not touched */
#endif
}

#endif /* USE_NATIVE_THREAD_PRIORITY */

static int
native_fd_select(int n, rb_fdset_t *readfds, rb_fdset_t *writefds, rb_fdset_t *exceptfds, struct timeval *timeout, rb_thread_t *th)
{
    return rb_fd_select(n, readfds, writefds, exceptfds, timeout);
}

static void
ubf_pthread_cond_signal(void *ptr)
{
    rb_thread_t *th = (rb_thread_t *)ptr;
    RUBY_DEBUG_LOG("th:%u on nt:%d", rb_th_serial(th), (int)th->nt->serial);
    rb_native_cond_signal(&th->nt->cond.intr);
}

static void
native_cond_sleep(rb_thread_t *th, rb_hrtime_t *rel)
{
    rb_nativethread_lock_t *lock = &th->interrupt_lock;
    rb_nativethread_cond_t *cond = &th->nt->cond.intr;

    /* Solaris cond_timedwait() return EINVAL if an argument is greater than
     * current_time + 100,000,000.  So cut up to 100,000,000.  This is
     * considered as a kind of spurious wakeup.  The caller to native_sleep
     * should care about spurious wakeup.
     *
     * See also [Bug #1341] [ruby-core:29702]
     * http://download.oracle.com/docs/cd/E19683-01/816-0216/6m6ngupgv/index.html
     */
    const rb_hrtime_t max = (rb_hrtime_t)100000000 * RB_HRTIME_PER_SEC;

    THREAD_BLOCKING_BEGIN(th);
    {
        rb_native_mutex_lock(lock);
        th->unblock.func = ubf_pthread_cond_signal;
        th->unblock.arg = th;

        if (RUBY_VM_INTERRUPTED(th->ec)) {
            /* interrupted.  return immediate */
            RUBY_DEBUG_LOG("interrupted before sleep th:%u", rb_th_serial(th));
        }
        else {
            if (!rel) {
                rb_native_cond_wait(cond, lock);
            }
            else {
                rb_hrtime_t end;

                if (*rel > max) {
                    *rel = max;
                }

                end = native_cond_timeout(cond, *rel);
                native_cond_timedwait(cond, lock, &end);
            }
        }
        th->unblock.func = 0;

        rb_native_mutex_unlock(lock);
    }
    THREAD_BLOCKING_END(th);

    RUBY_DEBUG_LOG("done th:%u", rb_th_serial(th));
}

#ifdef USE_UBF_LIST
static CCAN_LIST_HEAD(ubf_list_head);
static rb_nativethread_lock_t ubf_list_lock = RB_NATIVETHREAD_LOCK_INIT;

static void
ubf_list_atfork(void)
{
    ccan_list_head_init(&ubf_list_head);
    rb_native_mutex_initialize(&ubf_list_lock);
}

RBIMPL_ATTR_MAYBE_UNUSED()
static bool
ubf_list_contain_p(rb_thread_t *th)
{
    rb_thread_t *list_th;
    ccan_list_for_each(&ubf_list_head, list_th, sched.node.ubf) {
        if (list_th == th) return true;
    }
    return false;
}

/* The thread 'th' is registered to be trying unblock. */
static void
register_ubf_list(rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%u", rb_th_serial(th));
    struct ccan_list_node *node = &th->sched.node.ubf;

    VM_ASSERT(th->unblock.func != NULL);

    rb_native_mutex_lock(&ubf_list_lock);
    {
        // check not connected yet
        if (ccan_list_empty((struct ccan_list_head*)node)) {
            VM_ASSERT(!ubf_list_contain_p(th));
            ccan_list_add(&ubf_list_head, node);
        }
    }
    rb_native_mutex_unlock(&ubf_list_lock);

    timer_thread_wakeup();
}

/* The thread 'th' is unblocked. It no longer need to be registered. */
static void
unregister_ubf_list(rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%u", rb_th_serial(th));
    struct ccan_list_node *node = &th->sched.node.ubf;

    /* we can't allow re-entry into ubf_list_head */
    VM_ASSERT(th->unblock.func == NULL);
    rb_native_mutex_lock(&ubf_list_lock);
    {
        if (!ccan_list_empty((struct ccan_list_head*)node)) {
            VM_ASSERT(ubf_list_contain_p(th));
            ccan_list_del_init(node);
        }
    }
    rb_native_mutex_unlock(&ubf_list_lock);
}

/*
 * send a signal to intent that a target thread return from blocking syscall.
 * Maybe any signal is ok, but we chose SIGVTALRM.
 */
static void
ubf_wakeup_thread(rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%u thread_id:%p", rb_th_serial(th), (void *)th->nt->thread_id);

    pthread_kill(th->nt->thread_id, SIGVTALRM);
}

static void
ubf_select(void *ptr)
{
    rb_thread_t *th = (rb_thread_t *)ptr;
    RUBY_DEBUG_LOG("wakeup th:%u", rb_th_serial(th));
    ubf_wakeup_thread(th);
    register_ubf_list(th);
}

static bool
ubf_threads_empty(void)
{
    return ccan_list_empty(&ubf_list_head) != 0;
}

static void
ubf_wakeup_all_threads(void)
{
    if (!ubf_threads_empty()) {
        rb_thread_t *th;
        rb_native_mutex_lock(&ubf_list_lock);
        {
            ccan_list_for_each(&ubf_list_head, th, sched.node.ubf) {
                debug_threads(stderr, "timer thread in ufb_wakeup_all_threads: unblocking thread: %d\n", th->serial);
                ubf_wakeup_thread(th);
            }
        }
        rb_native_mutex_unlock(&ubf_list_lock);
    }
}

#else /* USE_UBF_LIST */
#define register_ubf_list(th) (void)(th)
#define unregister_ubf_list(th) (void)(th)
#define ubf_select 0
static void ubf_wakeup_all_threads(void) { return; }
static bool ubf_threads_empty(void) { return true; }
#define ubf_list_atfork() do {} while (0)
#endif /* USE_UBF_LIST */

#define TT_DEBUG 0
#define WRITE_CONST(fd, str) (void)(write((fd),(str),sizeof(str)-1)<0)

void
rb_thread_wakeup_timer_thread(int sig)
{
    // This function can be called from signal handlers so that
    // pthread_mutex_lock() should not be used.

    // wakeup timer thread
    timer_thread_wakeup_force();

    // interrupt main thread if main thread is available
    if (system_working) {
        rb_vm_t *vm = GET_VM();
        rb_thread_t *main_th = vm->ractor.main_thread;

        if (main_th) {
            volatile rb_execution_context_t *main_th_ec = ACCESS_ONCE(rb_execution_context_t *, main_th->ec);

            if (main_th_ec) {
                RUBY_VM_SET_TRAP_INTERRUPT(main_th_ec);

                if (vm->ubf_async_safe && main_th->unblock.func) {
                    (main_th->unblock.func)(main_th->unblock.arg);
                }
            }
        }
    }
}

#define CLOSE_INVALIDATE_PAIR(expr) \
    close_invalidate_pair(expr,"close_invalidate: "#expr)
static void
close_invalidate(int *fdp, const char *msg)
{
    int fd = *fdp;

    *fdp = -1;
    if (close(fd) < 0) {
        async_bug_fd(msg, errno, fd);
    }
}

static void
close_invalidate_pair(int fds[2], const char *msg)
{
    if (USE_EVENTFD && fds[0] == fds[1]) {
        fds[1] = -1; // disable write port first
        close_invalidate(&fds[0], msg);
    }
    else {
        close_invalidate(&fds[1], msg);
        close_invalidate(&fds[0], msg);
    }
}

static void
set_nonblock(int fd)
{
    int oflags;
    int err;

    oflags = fcntl(fd, F_GETFL);
    if (oflags == -1)
        rb_sys_fail(0);
    oflags |= O_NONBLOCK;
    err = fcntl(fd, F_SETFL, oflags);
    if (err == -1)
        rb_sys_fail(0);
}

/* communication pipe with timer thread and signal handler */
static void
setup_communication_pipe_internal(int pipes[2])
{
    int err;

    if (pipes[0] > 0 || pipes[1] > 0) {
        VM_ASSERT(pipes[0] > 0);
        VM_ASSERT(pipes[1] > 0);
        return;
    }

    /*
     * Don't bother with eventfd on ancient Linux 2.6.22..2.6.26 which were
     * missing EFD_* flags, they can fall back to pipe
     */
#if USE_EVENTFD && defined(EFD_NONBLOCK) && defined(EFD_CLOEXEC)
    pipes[0] = pipes[1] = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);

    if (pipes[0] >= 0) {
        rb_update_max_fd(pipes[0]);
        return;
    }
#endif

    err = rb_cloexec_pipe(pipes);
    if (err != 0) {
        rb_bug("can not create communication pipe");
    }
    rb_update_max_fd(pipes[0]);
    rb_update_max_fd(pipes[1]);
    set_nonblock(pipes[0]);
    set_nonblock(pipes[1]);
}

#if !defined(SET_CURRENT_THREAD_NAME) && defined(__linux__) && defined(PR_SET_NAME)
# define SET_CURRENT_THREAD_NAME(name) prctl(PR_SET_NAME, name)
#endif

enum {
    THREAD_NAME_MAX =
#if defined(__linux__)
    16
#elif defined(__APPLE__)
/* Undocumented, and main thread seems unlimited */
    64
#else
    16
#endif
};

static VALUE threadptr_invoke_proc_location(rb_thread_t *th);

static void
native_set_thread_name(rb_thread_t *th)
{
    return;
#ifdef SET_CURRENT_THREAD_NAME
    VALUE loc;
    if (!NIL_P(loc = th->name)) {
        SET_CURRENT_THREAD_NAME(RSTRING_PTR(loc));
    }
    else if ((loc = threadptr_invoke_proc_location(th)) != Qnil) {
        char *name, *p;
        char buf[THREAD_NAME_MAX];
        size_t len;
        int n;

        name = RSTRING_PTR(RARRAY_AREF(loc, 0));
        p = strrchr(name, '/'); /* show only the basename of the path. */
        if (p && p[1])
            name = p + 1;

        n = snprintf(buf, sizeof(buf), "%s:%d", name, NUM2INT(RARRAY_AREF(loc, 1)));
        RB_GC_GUARD(loc);

        len = (size_t)n;
        if (len >= sizeof(buf)) {
            buf[sizeof(buf)-2] = '*';
            buf[sizeof(buf)-1] = '\0';
        }
        SET_CURRENT_THREAD_NAME(buf);
    }
#endif
}

static void
native_set_another_thread_name(rb_nativethread_id_t thread_id, VALUE name)
{
#if defined SET_ANOTHER_THREAD_NAME || defined SET_CURRENT_THREAD_NAME
    char buf[THREAD_NAME_MAX];
    const char *s = "";
# if !defined SET_ANOTHER_THREAD_NAME
    if (!pthread_equal(pthread_self(), thread_id)) return;
# endif
    if (!NIL_P(name)) {
        long n;
        RSTRING_GETMEM(name, s, n);
        if (n >= (int)sizeof(buf)) {
            memcpy(buf, s, sizeof(buf)-1);
            buf[sizeof(buf)-1] = '\0';
            s = buf;
        }
    }
# if defined SET_ANOTHER_THREAD_NAME
    SET_ANOTHER_THREAD_NAME(thread_id, s);
# elif defined SET_CURRENT_THREAD_NAME
    SET_CURRENT_THREAD_NAME(s);
# endif
#endif
}

#if defined(RB_THREAD_T_HAS_NATIVE_ID) || defined(__APPLE__)
static VALUE
native_thread_native_thread_id(rb_thread_t *target_th)
{
    if (!target_th->nt) return Qnil;

#ifdef RB_THREAD_T_HAS_NATIVE_ID
    int tid = target_th->nt->tid;
    if (tid == 0) return Qnil;
    return INT2FIX(tid);
#elif defined(__APPLE__)
    uint64_t tid;
/* The first condition is needed because MAC_OS_X_VERSION_10_6
    is not defined on 10.5, and while __POWERPC__ takes care of ppc/ppc64,
    i386 will be broken without this. Note, 10.5 is supported with GCC upstream,
    so it has C++17 and everything needed to build modern Ruby. */
# if (!defined(MAC_OS_X_VERSION_10_6) || \
      (MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_6) || \
      defined(__POWERPC__) /* never defined for PowerPC platforms */)
    const bool no_pthread_threadid_np = true;
#   define NO_PTHREAD_MACH_THREAD_NP 1
# elif MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_6
    const bool no_pthread_threadid_np = false;
# else
#   if !(defined(__has_attribute) && __has_attribute(availability))
    /* __API_AVAILABLE macro does nothing on gcc */
    __attribute__((weak)) int pthread_threadid_np(pthread_t, uint64_t*);
#   endif
    /* Check weakly linked symbol */
    const bool no_pthread_threadid_np = !&pthread_threadid_np;
# endif
    if (no_pthread_threadid_np) {
        return ULL2NUM(pthread_mach_thread_np(pthread_self()));
    }
# ifndef NO_PTHREAD_MACH_THREAD_NP
    int e = pthread_threadid_np(target_th->nt->thread_id, &tid);
    if (e != 0) rb_syserr_fail(e, "pthread_threadid_np");
    return ULL2NUM((unsigned long long)tid);
# endif
#endif
}
# define USE_NATIVE_THREAD_NATIVE_THREAD_ID 1
#else
# define USE_NATIVE_THREAD_NATIVE_THREAD_ID 0
#endif

static struct {
    rb_serial_t created_fork_gen;
    pthread_t pthread_id;

    int comm_fds[2]; // r, w

#if (HAVE_SYS_EPOLL_H || HAVE_SYS_EVENT_H) && USE_MN_THREADS
    int event_fd; // kernel event queue fd (epoll/kqueue)
#endif
#if HAVE_SYS_EPOLL_H && USE_MN_THREADS
#define EPOLL_EVENTS_MAX 0x10
    struct epoll_event finished_events[EPOLL_EVENTS_MAX];
#elif HAVE_SYS_EVENT_H && USE_MN_THREADS
#define KQUEUE_EVENTS_MAX 0x10
    struct kevent finished_events[KQUEUE_EVENTS_MAX];
#endif

    // waiting threads list
    struct ccan_list_head waiting; // waiting threads in ractors
    pthread_mutex_t waiting_lock;
} timer_th = {
    .created_fork_gen = 0,
};

#define TIMER_THREAD_CREATED_P() (timer_th.created_fork_gen == current_fork_gen)

static void timer_thread_check_timeslice(rb_vm_t *vm);
static int timer_thread_set_timeout(rb_vm_t *vm);
static void timer_thread_wakeup_thread(rb_thread_t *th);

#include "thread_pthread_mn.c"

static rb_thread_t *
thread_sched_waiting_thread(struct rb_thread_sched_waiting *w)
{
    if (w) {
        return (rb_thread_t *)((size_t)w - offsetof(rb_thread_t, sched.waiting_reason));
    }
    else {
        return NULL;
    }
}

// timeout in ms
static int
timer_thread_set_timeout(rb_vm_t *vm)
{
#if 0
    return 10; // ms
#else
    int timeout = -1;

    ractor_sched_lock(vm, NULL); // TODO: remove
    {
        if (   !ccan_list_empty(&vm->ractor.sched.timeslice_threads) // (1-1) Provide time slice for active NTs
            || !ubf_threads_empty()                                  // (1-3) Periodic UBF
            || vm->ractor.sched.grq_cnt > 0                          // (1-4) Lazy GRQ deq start
            ) {

            RUBY_DEBUG_LOG("timeslice:%d ubf:%d grq:%d",
                           !ccan_list_empty(&vm->ractor.sched.timeslice_threads),
                           !ubf_threads_empty(),
                           (vm->ractor.sched.grq_cnt > 0));

            timeout = TIME_TIMEOUT_MSEC; // ms
            vm->ractor.sched.timeslice_wait_inf = false;
        }
        else {
            vm->ractor.sched.timeslice_wait_inf = true;
        }
    }
    ractor_sched_unlock(vm, NULL);

    if (vm->ractor.sched.timeslice_wait_inf) {
        rb_native_mutex_lock(&timer_th.waiting_lock);
        {
            struct rb_thread_sched_waiting *w = ccan_list_top(&timer_th.waiting, struct rb_thread_sched_waiting, node);
            rb_thread_t *th = thread_sched_waiting_thread(w);

            if (th && (th->sched.waiting_reason.flags & thread_sched_waiting_timeout)) {
                rb_hrtime_t now = rb_hrtime_now();
                rb_hrtime_t hrrel = rb_hrtime_sub(th->sched.waiting_reason.data.timeout, now);

                RUBY_DEBUG_LOG("th:%u now:%lu rel:%lu", rb_th_serial(th), (unsigned long)now, (unsigned long)hrrel);

                // TODO: overflow?
                timeout = (int)((hrrel + RB_HRTIME_PER_MSEC - 1) / RB_HRTIME_PER_MSEC); // ms
                if (timeout > TIME_TIMEOUT_MAX_MS) {
                    timeout = TIME_TIMEOUT_MAX_MS;
                }
            }
        }
        rb_native_mutex_unlock(&timer_th.waiting_lock);
    }

    RUBY_DEBUG_LOG("timeout:%d inf:%d", timeout, (int)vm->ractor.sched.timeslice_wait_inf);

    // fprintf(stderr, "timeout:%d\n", timeout);
    debug_threads(stderr, "Timer thread timeout in ms: %d\n", timeout);
    return timeout;
#endif
}

static void
timer_thread_check_signal(rb_vm_t *vm)
{
    // ruby_sigchld_handler(vm); TODO

    int signum = rb_signal_buff_size();
    if (UNLIKELY(signum > 0) && vm->ractor.main_thread) {
        RUBY_DEBUG_LOG("signum:%d", signum);
        debug_threads(stderr, "timer_thread_check_signal: setting trap interrupt on main thread due to sig_buff_size > 0\n");
        threadptr_trap_interrupt(vm->ractor.main_thread);
    }
}

static bool
timer_thread_check_exceed(rb_hrtime_t abs, rb_hrtime_t now)
{
    if (abs < now) {
        return true;
    }
    else if (abs - now < RB_HRTIME_PER_MSEC) {
        return true; // too short time
    }
    else {
        return false;
    }
}

static rb_thread_t *
timer_thread_deq_wakeup(rb_vm_t *vm, rb_hrtime_t now)
{
    struct rb_thread_sched_waiting *w = ccan_list_top(&timer_th.waiting, struct rb_thread_sched_waiting, node);

    if (w != NULL &&
        (w->flags & thread_sched_waiting_timeout) &&
        timer_thread_check_exceed(w->data.timeout, now)) {

        RUBY_DEBUG_LOG("wakeup th:%u", rb_th_serial(thread_sched_waiting_thread(w)));

        // delete from waiting list
        ccan_list_del_init(&w->node);

        // setup result
        w->flags = thread_sched_waiting_none;
        w->data.result = 0;

        return thread_sched_waiting_thread(w);
    }

    return NULL;
}

static void
timer_thread_wakeup_thread(rb_thread_t *th)
{
    RUBY_DEBUG_LOG("th:%u", rb_th_serial(th));
    struct rb_thread_sched *sched = TH_SCHED(th);

    thread_sched_lock(sched, th);
    {
        if (sched->running != th) {
            debug_threads(stderr, "timer_thread_wakeup_thread: thread_sched_to_ready_common for th:%d r:%d\n",
                th->serial, rb_ractor_id(th->ractor)
            );
            thread_sched_to_ready_common(sched, th, true, false);
        }
        else {
            debug_threads(stderr, "timer_thread_wakeup_thread: would call thread_sched_to_ready for th:%d r:%d, but already running!\n",
                th->serial, rb_ractor_id(th->ractor)
            );
            // will be release the execution right
        }
    }
    thread_sched_unlock(sched, th);
}

static void
timer_thread_check_timeout(rb_vm_t *vm)
{
    rb_hrtime_t now = rb_hrtime_now();
    rb_thread_t *th;

    rb_native_mutex_lock(&timer_th.waiting_lock);
    {
        while ((th = timer_thread_deq_wakeup(vm, now)) != NULL) {
            debug_threads(stderr, "timer_thread_check_timeout: wakeup th:%d r:%d\n",
                th->serial, rb_ractor_id(th->ractor)
            );
            timer_thread_wakeup_thread(th);
        }
    }
    rb_native_mutex_unlock(&timer_th.waiting_lock);
}

// TODO: Luke: implement time checking. This is wasting lots of time
static void
timer_thread_check_timeslice(rb_vm_t *vm)
{
    // TODO: check time
    rb_thread_t *th;
    ccan_list_for_each(&vm->ractor.sched.timeslice_threads, th, sched.node.timeslice_threads) {
        /*uint32_t limits_us = TIME_QUANTUM_USEC;*/
        /*if (th->status != THREAD_RUNNABLE) { // shouldn't be in this situation, so set an interrupt*/
            /*th->running_time_us += limits_us;*/
            /*RUBY_VM_SET_TIMER_INTERRUPT(th->ec);*/
            /*continue;*/
        /*}*/
        /*if (th->priority > 0) {*/
            /*limits_us <<= th->priority;*/
        /*} else {*/
            /*limits_us >>= -th->priority;*/
        /*}*/
        /*if ((th->running_time_us + TIME_TIMEOUT_USEC) >= limits_us) {*/
            RUBY_DEBUG_LOG("timeslice th:%u", rb_th_serial(th));
            debug_threads(stderr, "timer_thread_check_timeslice: setting timer interrupt on th:%d on r:%d (%zu ms)\n",
                th->serial, rb_ractor_id(th->ractor), (th->running_time_us + TIME_TIMEOUT_USEC) / 1000
            );
            /*th->running_time_us += TIME_TIMEOUT_USEC;*/
            RUBY_VM_SET_TIMER_INTERRUPT(th->ec);
        /*} else {*/
            /*th->running_time_us += TIME_TIMEOUT_USEC;*/
            /*debug_threads(stderr, "timer_thread_check_timeslice: no timer interrupt on th:%d on r:%d (%zu ms)\n",*/
                /*th->serial, rb_ractor_id(th->ractor), th->running_time_us / 1000*/
            /*);*/
        /*}*/
    }
}

void
rb_assert_sig(void)
{
    sigset_t oldmask;
    pthread_sigmask(0, NULL, &oldmask);
    if (sigismember(&oldmask, SIGVTALRM)) {
        rb_bug("!!!");
    }
    else {
        RUBY_DEBUG_LOG("ok");
    }
}

static void *
timer_thread_func(void *ptr)
{
    rb_vm_t *vm = (rb_vm_t *)ptr;
#ifdef RUBY_NT_SERIAL
    ruby_nt_serial = (rb_atomic_t)-1;
#endif
    int tid = get_native_thread_id();
    timer_thread_tid = tid;
    debug_threads(stderr, "Timer thread start: id=%d\n", tid);

    RUBY_DEBUG_LOG("started%s", "");

    while (system_working) {
        debug_threads(stderr, "Timer thread loop start\n");
        timer_thread_check_signal(vm);
        timer_thread_check_timeout(vm);
        ubf_wakeup_all_threads();

        RUBY_DEBUG_LOG("system_working:%d", system_working);
        timer_thread_polling(vm);
    }

    debug_threads(stderr, "Timer thread end\n");
    RUBY_DEBUG_LOG("terminated");
    return NULL;
}

/* only use signal-safe system calls here */
static void
signal_communication_pipe(int fd)
{
#if USE_EVENTFD
    const uint64_t buff = 1;
#else
    const char buff = '!';
#endif
    ssize_t result;

    /* already opened */
    if (fd >= 0) {
      retry:
        if ((result = write(fd, &buff, sizeof(buff))) <= 0) {
            int e = errno;
            switch (e) {
              case EINTR: goto retry;
              case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
              case EWOULDBLOCK:
#endif
                break;
              default:
                async_bug_fd("rb_thread_wakeup_timer_thread: write", e, fd);
            }
        }
        if (TT_DEBUG) WRITE_CONST(2, "rb_thread_wakeup_timer_thread: write\n");
    }
    else {
        // ignore wakeup
    }
}

// wake up the timer thread
static void
timer_thread_wakeup_force(void)
{
    // should not use RUBY_DEBUG_LOG() because it can be called within signal handlers.
    signal_communication_pipe(timer_th.comm_fds[1]);
}

static void
timer_thread_wakeup_locked(rb_vm_t *vm, bool needs_lock)
{
    // should be locked before.
    if (needs_lock) ASSERT_ractor_sched_locked(vm, NULL);

    if (timer_th.created_fork_gen == current_fork_gen) {
        if (vm->ractor.sched.timeslice_wait_inf) {
            RUBY_DEBUG_LOG("wakeup with fd:%d", timer_th.comm_fds[1]);
            debug_threads(stderr, "timer_thread_wakeup_locked: wakeup_force (waking up timer thread)\n");
            timer_thread_wakeup_force();
        }
        else {
            RUBY_DEBUG_LOG("will be wakeup...");
        }
    }
}

static void
timer_thread_wakeup(void)
{
    rb_vm_t *vm = GET_VM();

    ractor_sched_lock(vm, NULL);
    {
        timer_thread_wakeup_locked(vm, true);
    }
    ractor_sched_unlock(vm, NULL);
}

static void
rb_thread_create_timer_thread(void)
{
    rb_serial_t created_fork_gen = timer_th.created_fork_gen;

    RUBY_DEBUG_LOG("fork_gen create:%d current:%d", (int)created_fork_gen, (int)current_fork_gen);

    timer_th.created_fork_gen = current_fork_gen;

    if (created_fork_gen != current_fork_gen) {
        if (created_fork_gen != 0) {
            RUBY_DEBUG_LOG("forked child process");

            CLOSE_INVALIDATE_PAIR(timer_th.comm_fds);
#if HAVE_SYS_EPOLL_H && USE_MN_THREADS
            close_invalidate(&timer_th.event_fd, "close event_fd");
#endif
            rb_native_mutex_destroy(&timer_th.waiting_lock);
        }

        ccan_list_head_init(&timer_th.waiting);
        rb_native_mutex_initialize(&timer_th.waiting_lock);

        // open communication channel
        setup_communication_pipe_internal(timer_th.comm_fds);

        // open event fd
        timer_thread_setup_mn();
    }

    pthread_create(&timer_th.pthread_id, NULL, timer_thread_func, GET_VM());
}

static int
native_stop_timer_thread(void)
{
    int stopped;
    stopped = --system_working <= 0;

    if (stopped) {
        RUBY_DEBUG_LOG("wakeup send %d", timer_th.comm_fds[1]);
        timer_thread_wakeup_force();
        RUBY_DEBUG_LOG("wakeup sent");
        pthread_join(timer_th.pthread_id, NULL);
    }

    if (TT_DEBUG) fprintf(stderr, "stop timer thread\n");
    return stopped;
}

static void
native_reset_timer_thread(void)
{
    //
}

#ifdef HAVE_SIGALTSTACK
int
ruby_stack_overflowed_p(const rb_thread_t *th, const void *addr)
{
    void *base;
    size_t size;
    const size_t water_mark = 1024 * 1024;
    STACK_GROW_DIR_DETECTION;

#ifdef STACKADDR_AVAILABLE
    if (get_stack(&base, &size) == 0) {
# ifdef __APPLE__
        if (pthread_equal(th->nt->thread_id, native_main_thread.id)) {
            struct rlimit rlim;
            if (getrlimit(RLIMIT_STACK, &rlim) == 0 && rlim.rlim_cur > size) {
                size = (size_t)rlim.rlim_cur;
            }
        }
# endif
        base = (char *)base + STACK_DIR_UPPER(+size, -size);
    }
    else
#endif
    if (th) {
        size = th->ec->machine.stack_maxsize;
        base = (char *)th->ec->machine.stack_start - STACK_DIR_UPPER(0, size);
    }
    else {
        return 0;
    }
    size /= RUBY_STACK_SPACE_RATIO;
    if (size > water_mark) size = water_mark;
    if (IS_STACK_DIR_UPPER()) {
        if (size > ~(size_t)base+1) size = ~(size_t)base+1;
        if (addr > base && addr <= (void *)((char *)base + size)) return 1;
    }
    else {
        if (size > (size_t)base) size = (size_t)base;
        if (addr > (void *)((char *)base - size) && addr <= base) return 1;
    }
    return 0;
}
#endif

int
rb_reserved_fd_p(int fd)
{
    /* no false-positive if out-of-FD at startup */
    if (fd < 0) return 0;

    if (fd == timer_th.comm_fds[0] ||
        fd == timer_th.comm_fds[1]
#if (HAVE_SYS_EPOLL_H || HAVE_SYS_EVENT_H) && USE_MN_THREADS
        || fd == timer_th.event_fd
#endif
        ) {
        goto check_fork_gen;
    }
    return 0;

  check_fork_gen:
    if (timer_th.created_fork_gen == current_fork_gen) {
        /* async-signal-safe */
        return 1;
    }
    else {
        return 0;
    }
}

rb_nativethread_id_t
rb_nativethread_self(void)
{
    return pthread_self();
}

#if defined(USE_POLL) && !defined(HAVE_PPOLL)
/* TODO: don't ignore sigmask */
static int
ruby_ppoll(struct pollfd *fds, nfds_t nfds,
      const struct timespec *ts, const sigset_t *sigmask)
{
    int timeout_ms;

    if (ts) {
        int tmp, tmp2;

        if (ts->tv_sec > INT_MAX/1000)
            timeout_ms = INT_MAX;
        else {
            tmp = (int)(ts->tv_sec * 1000);
            /* round up 1ns to 1ms to avoid excessive wakeups for <1ms sleep */
            tmp2 = (int)((ts->tv_nsec + 999999L) / (1000L * 1000L));
            if (INT_MAX - tmp < tmp2)
                timeout_ms = INT_MAX;
            else
                timeout_ms = (int)(tmp + tmp2);
        }
    }
    else
        timeout_ms = -1;

    return poll(fds, nfds, timeout_ms);
}
#  define ppoll(fds,nfds,ts,sigmask) ruby_ppoll((fds),(nfds),(ts),(sigmask))
#endif

/*
 * Single CPU setups benefit from explicit sched_yield() before ppoll(),
 * since threads may be too starved to enter the GVL waitqueue for
 * us to detect contention.  Instead, we want to kick other threads
 * so they can run and possibly prevent us from entering slow paths
 * in ppoll() or similar syscalls.
 *
 * Confirmed on FreeBSD 11.2 and Linux 4.19.
 * [ruby-core:90417] [Bug #15398]
 */
#define THREAD_BLOCKING_YIELD(th) do { \
    const rb_thread_t *next_th; \
    struct rb_thread_sched *sched = TH_SCHED(th); \
    RB_VM_SAVE_MACHINE_CONTEXT(th); \
    thread_sched_to_waiting(sched, (th), true); \
    next_th = sched->running; \
    rb_native_mutex_unlock(&sched->lock_); \
    debug_threads(stderr, "sched_yield()\n");\
    native_thread_yield(); /* TODO: needed? */ \
    if (!next_th && rb_ractor_living_thread_num(th->ractor) > 1) { \
        debug_threads(stderr, "sched_yield()\n");\
        native_thread_yield(); \
    }

static void
native_sleep(rb_thread_t *th, rb_hrtime_t *rel)
{
    struct rb_thread_sched *sched = TH_SCHED(th);

    RUBY_DEBUG_LOG("rel:%d", rel ? (int)*rel : 0);
    if (rel) {
        if (th_has_dedicated_nt(th)) {
            debug_threads(stderr, "native_sleep with timeout before th:%d dnt:%d\n", th->serial, th->nt->serial);
            native_cond_sleep(th, rel);
            debug_threads(stderr, "native_sleep with timeout before th:%d dnt:%d\n", th->serial, th->nt->serial);
        }
        else {
            debug_threads(stderr, "native_sleep with timeout before th:%d snt:%d\n", th->serial, th->nt->serial);
            thread_sched_wait_events(sched, th, -1, thread_sched_waiting_timeout, rel);
            debug_threads(stderr, "native_sleep with timeout after th:%d snt:%d\n", th->serial, th->nt->serial);
        }
    }
    else {
        debug_threads(stderr, "native_sleep !timeout before (sleep) th:%d\n", th->serial);
        thread_sched_to_waiting_until_wakeup(sched, th);
        debug_threads(stderr, "native_sleep !timeout after (wakeup) th:%d\n", th->serial);
    }

    RUBY_DEBUG_LOG("wakeup");
}

// fork read-write lock (only for pthread)
static pthread_rwlock_t rb_thread_fork_rw_lock = PTHREAD_RWLOCK_INITIALIZER;

void
rb_thread_release_fork_lock(void)
{
    int r;
    if ((r = pthread_rwlock_unlock(&rb_thread_fork_rw_lock))) {
        rb_bug_errno("pthread_rwlock_unlock", r);
    }
}

void
rb_thread_reset_fork_lock(void)
{
    int r;
    if ((r = pthread_rwlock_destroy(&rb_thread_fork_rw_lock))) {
        rb_bug_errno("pthread_rwlock_destroy", r);
    }

    if ((r = pthread_rwlock_init(&rb_thread_fork_rw_lock, NULL))) {
        rb_bug_errno("pthread_rwlock_init", r);
    }
}

void *
rb_thread_prevent_fork(void *(*func)(void *), void *data)
{
    int r;
    if ((r = pthread_rwlock_rdlock(&rb_thread_fork_rw_lock))) {
        rb_bug_errno("pthread_rwlock_rdlock", r);
    }
    void *result = func(data);
    rb_thread_release_fork_lock();
    return result;
}

void
rb_thread_acquire_fork_lock(void)
{
    int r;
    if ((r = pthread_rwlock_wrlock(&rb_thread_fork_rw_lock))) {
        rb_bug_errno("pthread_rwlock_wrlock", r);
    }
}

// thread internal event hooks (only for pthread)

struct rb_internal_thread_event_hook {
    rb_internal_thread_event_callback callback;
    rb_event_flag_t event;
    void *user_data;

    struct rb_internal_thread_event_hook *next;
};

static pthread_rwlock_t rb_internal_thread_event_hooks_rw_lock = PTHREAD_RWLOCK_INITIALIZER;

rb_internal_thread_event_hook_t *
rb_internal_thread_add_event_hook(rb_internal_thread_event_callback callback, rb_event_flag_t internal_event, void *user_data)
{
    rb_internal_thread_event_hook_t *hook = ALLOC_N(rb_internal_thread_event_hook_t, 1);
    hook->callback = callback;
    hook->user_data = user_data;
    hook->event = internal_event;

    int r;
    if ((r = pthread_rwlock_wrlock(&rb_internal_thread_event_hooks_rw_lock))) {
        rb_bug_errno("pthread_rwlock_wrlock", r);
    }

    hook->next = rb_internal_thread_event_hooks;
    ATOMIC_PTR_EXCHANGE(rb_internal_thread_event_hooks, hook);

    if ((r = pthread_rwlock_unlock(&rb_internal_thread_event_hooks_rw_lock))) {
        rb_bug_errno("pthread_rwlock_unlock", r);
    }
    return hook;
}

bool
rb_internal_thread_remove_event_hook(rb_internal_thread_event_hook_t * hook)
{
    int r;
    if ((r = pthread_rwlock_wrlock(&rb_internal_thread_event_hooks_rw_lock))) {
        rb_bug_errno("pthread_rwlock_wrlock", r);
    }

    bool success = FALSE;

    if (rb_internal_thread_event_hooks == hook) {
        ATOMIC_PTR_EXCHANGE(rb_internal_thread_event_hooks, hook->next);
        success = TRUE;
    }
    else {
        rb_internal_thread_event_hook_t *h = rb_internal_thread_event_hooks;

        do {
            if (h->next == hook) {
                h->next = hook->next;
                success = TRUE;
                break;
            }
        } while ((h = h->next));
    }

    if ((r = pthread_rwlock_unlock(&rb_internal_thread_event_hooks_rw_lock))) {
        rb_bug_errno("pthread_rwlock_unlock", r);
    }

    if (success) {
        ruby_xfree(hook);
    }
    return success;
}

static void
rb_thread_execute_hooks(rb_event_flag_t event, rb_thread_t *th)
{
    int r;
    if ((r = pthread_rwlock_rdlock(&rb_internal_thread_event_hooks_rw_lock))) {
        rb_bug_errno("pthread_rwlock_rdlock", r);
    }

    if (rb_internal_thread_event_hooks) {
        rb_internal_thread_event_hook_t *h = rb_internal_thread_event_hooks;
        do {
            if (h->event & event) {
                rb_internal_thread_event_data_t event_data = {
                    .thread = th->self,
                };
                (*h->callback)(event, &event_data, h->user_data);
            }
        } while((h = h->next));
    }
    if ((r = pthread_rwlock_unlock(&rb_internal_thread_event_hooks_rw_lock))) {
        rb_bug_errno("pthread_rwlock_unlock", r);
    }
}

// return true if the current thread acquires DNT.
// return false if the current thread already acquires DNT.
bool
rb_thread_lock_native_thread(void)
{
    rb_thread_t *th = GET_THREAD();
    bool is_snt = !th->nt->is_permanent_dnt && th->nt->dedicated == 0;
    native_thread_dedicated_inc(th->vm, th->ractor, th->nt);

    return is_snt;
}

#endif /* THREAD_SYSTEM_DEPENDENT_IMPLEMENTATION */
