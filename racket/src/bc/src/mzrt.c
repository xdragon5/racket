#include "schpriv.h"

#ifdef MZ_USE_MZRT

/************************************************************************/
/************************************************************************/
/************************************************************************/
#include "schgc.h"

THREAD_LOCAL_DECL(mz_proc_thread *proc_thread_self);

#ifdef MZ_XFORM
START_XFORM_SUSPEND;
#endif

/* std C headers */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <../sconfig.h>

/* platform headers */
#ifdef WIN32
# include <windows.h>
# include <process.h>
#else
# include <pthread.h>
# include <signal.h>
# include <unistd.h>
# include <time.h>
#endif
#ifdef UNIX_FIND_STACK_BOUNDS
#include <sys/time.h>
#include <sys/resource.h>
#endif

void mzrt_sleep(int seconds)
{
#ifdef WIN32
  Sleep(seconds * 1000);
#else
  struct timespec set;
  struct timespec rem;
  set.tv_sec  = seconds;
  set.tv_nsec = 0;
  rem.tv_sec  = 0;
  rem.tv_nsec = 0;
  while ((-1 == nanosleep(&set, &rem))) {
    /* fprintf(stderr, "%i %i INITIAL\n", set.tv_sec, set.tv_nsec); */
    /* fprintf(stderr, "%i %i LEFT\n", rem.tv_sec, rem.tv_nsec); */
    set = rem;
    /* fprintf(stderr, "%i %i NOW\n", set.tv_sec, set.tv_nsec); */
  }
#endif
}

/***********************************************************************/
/*                Threads                                              */
/***********************************************************************/
typedef struct mzrt_thread_stub_data {
  mz_proc_thread_start start_proc;
  void *data;
  mz_proc_thread *thread;
} mzrt_thread_stub_data;

void *mzrt_thread_stub(void *data){
  mzrt_thread_stub_data *stub_data  = (mzrt_thread_stub_data*) data;
  mz_proc_thread_start start_proc     = stub_data->start_proc;
  void *start_proc_data               = stub_data->data;
  void* res;

  scheme_init_os_thread();

  proc_thread_self = stub_data->thread;

  free(data);

  res = start_proc(start_proc_data);

#ifdef WIN32
  proc_thread_self->res = res;
#endif

  if (!--proc_thread_self->refcount)
    free(proc_thread_self);

  scheme_done_os_thread();

  return res;
}

#ifdef WIN32
unsigned int WINAPI mzrt_win_thread_stub(void *data)
{
  (void)mzrt_thread_stub(data);
  return 0;
}
#endif


mzrt_os_thread_id mz_proc_os_thread_self() {
#ifdef WIN32
  /* For Windows, this result is not compatible with mz_proc_thread_id(),
     so don't mix them up! */
  return GetCurrentThreadId();
#else
  return pthread_self();
#endif
}

mzrt_thread_id mz_proc_thread_id(mz_proc_thread* thread) {

  return thread->threadid;
}

mz_proc_thread* mzrt_proc_first_thread_init() {
  /* initialize mz_proc_thread struct for first thread that wasn't created with mz_proc_thread_create */
  mz_proc_thread *thread = (mz_proc_thread*)malloc(sizeof(mz_proc_thread));
#ifdef WIN32
  /* This pseudo-handle is not valid as a reference from any other thread,
     but it will be distinct from all other IDs: */
  thread->threadid  = GetCurrentThread();
#else
  thread->threadid  = mz_proc_os_thread_self();
#endif
  
  proc_thread_self  = thread;
  thread->refcount = 1;
  return thread;
}

mz_proc_thread* mz_proc_thread_create_w_stacksize(mz_proc_thread_start start_proc, void* data, intptr_t stacksize)
{
  mz_proc_thread *thread = (mz_proc_thread*)malloc(sizeof(mz_proc_thread));
  mzrt_thread_stub_data *stub_data;
  int ok;

#   ifndef WIN32
  pthread_attr_t *attr;
  pthread_attr_t attr_storage;

  if (stacksize) {
    attr = &attr_storage;
    pthread_attr_init(attr);
    pthread_attr_setstacksize(attr, stacksize); /*8MB*/
  } else
    attr = NULL;
#   endif

  thread->refcount = 2;

  stub_data = (mzrt_thread_stub_data*)malloc(sizeof(mzrt_thread_stub_data));

  stub_data->start_proc = start_proc;
  stub_data->data       = data;
  stub_data->thread     = thread;
#   ifdef WIN32
  thread->threadid = (HANDLE)_beginthreadex(NULL, stacksize, mzrt_win_thread_stub, stub_data, 0, NULL);
  ok = (thread->threadid != (HANDLE)-1L);
#   else
  ok = !pthread_create(&thread->threadid, attr, mzrt_thread_stub, stub_data);
#   endif

  if (!ok) {
    free(thread);
    free(stub_data);
    return NULL;
  }

  return thread;
}

mz_proc_thread* mz_proc_thread_create(mz_proc_thread_start start_proc, void* data) {
  uintptr_t stacksize;

#if defined(ASSUME_FIXED_STACK_SIZE)
  stacksize = FIXED_STACK_SIZE;
#elif defined(UNIX_FIND_STACK_BOUNDS)
  {
    struct rlimit rl;
    getrlimit(RLIMIT_STACK, &rl);
    stacksize = (uintptr_t)rl.rlim_cur;
#  ifdef UNIX_STACK_MAXIMUM
    if (stacksize > UNIX_STACK_MAXIMUM)
      stacksize = UNIX_STACK_MAXIMUM;
#  endif
  }
#else
  stacksize = 0;
#endif

  return mz_proc_thread_create_w_stacksize(start_proc, data, stacksize);
}

void * mz_proc_thread_wait(mz_proc_thread *thread) {
  void *rc;
#ifdef WIN32
  WaitForSingleObject(thread->threadid,INFINITE);
  rc = proc_thread_self->res;
  CloseHandle(thread->threadid);
#else
  pthread_join(thread->threadid, &rc);
#endif

  if (!--thread->refcount)
    free(thread);
  
  return rc;
}

int mz_proc_thread_detach(mz_proc_thread *thread) {
  int rc;
#ifdef WIN32
  rc = CloseHandle(thread->threadid);
#else
  rc = pthread_detach(thread->threadid);
#endif

  if (!--thread->refcount)
    free(thread);

  return rc;
}

void mz_proc_thread_exit(void *rc) {
#ifdef WIN32
  proc_thread_self->res = rc;
  _endthreadex(0);
#else
#   ifndef MZ_PRECISE_GC
  pthread_exit(rc);
#   else
  pthread_exit(rc);
#   endif
#endif
}

/***********************************************************************/
/*                RW Lock                                              */
/***********************************************************************/

/* Unix **************************************************************/

#ifndef WIN32

# ifdef HAVE_PTHREAD_RWLOCK

struct mzrt_rwlock {
  pthread_rwlock_t lock;
};

int mzrt_rwlock_create(mzrt_rwlock **lock) {
  *lock = malloc(sizeof(mzrt_rwlock));
  return pthread_rwlock_init(&(*lock)->lock, NULL);
}

int mzrt_rwlock_rdlock(mzrt_rwlock *lock) {
  return pthread_rwlock_rdlock(&lock->lock);
}

int mzrt_rwlock_wrlock(mzrt_rwlock *lock) {
  return pthread_rwlock_wrlock(&lock->lock);
}

int mzrt_rwlock_tryrdlock(mzrt_rwlock *lock) {
  return pthread_rwlock_tryrdlock(&lock->lock);
}

int mzrt_rwlock_trywrlock(mzrt_rwlock *lock) {
  return pthread_rwlock_trywrlock(&lock->lock);
}
int mzrt_rwlock_unlock(mzrt_rwlock *lock) {
  return pthread_rwlock_unlock(&lock->lock);
}

int mzrt_rwlock_destroy(mzrt_rwlock *lock) {
  return pthread_rwlock_destroy(&lock->lock);
}

# else

struct mzrt_rwlock {
  pthread_mutex_t m;
  pthread_cond_t cr, cw;
  int readers, writers, write_waiting;
};

int mzrt_rwlock_create(mzrt_rwlock **lock) {
  int err;

  *lock = malloc(sizeof(mzrt_rwlock));
  err = pthread_mutex_init(&(*lock)->m, NULL);
  if (err) { free(*lock); return err; }
  err = pthread_cond_init(&(*lock)->cr, NULL);
  if (err) { free(*lock); return err; }
  err = pthread_cond_init(&(*lock)->cw, NULL);
  if (err) { free(*lock); return err; }
  return err;
}

static int rwlock_rdlock(mzrt_rwlock *lock, int just_try) {
  int err;

  err = pthread_mutex_lock(&lock->m);
  if (err) return err;
  while (lock->writers || lock->write_waiting) {
    if (just_try) {
      err = pthread_mutex_unlock(&lock->m);
      if (err) return err;
      return EBUSY;
    } else {
      err = pthread_cond_wait(&lock->cr, &lock->m);
      if (err)
        return err;
    }
  }
  lock->readers++;
  return pthread_mutex_unlock(&lock->m);
}

int mzrt_rwlock_rdlock(mzrt_rwlock *lock) {
  return rwlock_rdlock(lock, 0);
}

static int rwlock_wrlock(mzrt_rwlock *lock, int just_try) {
  int err;

  err = pthread_mutex_lock(&lock->m);
  if (err) return err;
  while (lock->writers || lock->readers) {
    if (just_try) {
      err = pthread_mutex_unlock(&lock->m);
      if (err) return err;
      return EBUSY;
    } else {
      lock->write_waiting++;
      err = pthread_cond_wait(&lock->cw, &lock->m);
      --lock->write_waiting;
      if (err)
        return err;
    }
  }
  lock->writers++;
  return pthread_mutex_unlock(&lock->m);
}

int mzrt_rwlock_wrlock(mzrt_rwlock *lock) {
  return rwlock_wrlock(lock, 0);
}

int mzrt_rwlock_tryrdlock(mzrt_rwlock *lock) {
  return rwlock_rdlock(lock, 1);
}

int mzrt_rwlock_trywrlock(mzrt_rwlock *lock) {
  return rwlock_wrlock(lock, 1);
}

int mzrt_rwlock_unlock(mzrt_rwlock *lock) {
  int err;

  err = pthread_mutex_lock(&lock->m);
  if (err) return err;

  if (lock->readers)
    --lock->readers; /* must have been a read lock */
  else
    --lock->writers;

  if (lock->write_waiting)
    err = pthread_cond_signal(&lock->cw);
  else
    err = pthread_cond_broadcast(&lock->cr);
  if (err) return err;
  
  return pthread_mutex_unlock(&lock->m);
}

int mzrt_rwlock_destroy(mzrt_rwlock *lock) {
  int r = 0;

  r |= pthread_mutex_destroy(&lock->m);
  r |= pthread_cond_destroy(&lock->cr);
  r |= pthread_cond_destroy(&lock->cw);

  if (!r) free(lock);

  return r;
}

# endif

struct mzrt_mutex {
  pthread_mutex_t mutex;
};

int mzrt_mutex_create(mzrt_mutex **mutex) {
  *mutex = malloc(sizeof(struct mzrt_mutex));
  return pthread_mutex_init(&(*mutex)->mutex, NULL);
}

int mzrt_mutex_lock(mzrt_mutex *mutex) {
  return pthread_mutex_lock(&mutex->mutex);
}

int mzrt_mutex_trylock(mzrt_mutex *mutex) {
  return pthread_mutex_trylock(&mutex->mutex);
}

int mzrt_mutex_unlock(mzrt_mutex *mutex) {
  return pthread_mutex_unlock(&mutex->mutex);
}

int mzrt_mutex_destroy(mzrt_mutex *mutex) {
  int r;
  r = pthread_mutex_destroy(&mutex->mutex);
  if (!r) free(mutex);
  return r;
}

struct mzrt_cond {
  pthread_cond_t cond;
};

int mzrt_cond_create(mzrt_cond **cond) {
  *cond = malloc(sizeof(struct mzrt_cond));
  return pthread_cond_init(&(*cond)->cond, NULL);
}

int mzrt_cond_wait(mzrt_cond *cond, mzrt_mutex *mutex) {
  return pthread_cond_wait(&cond->cond, &mutex->mutex);
}

int mzrt_cond_timedwait(mzrt_cond *cond, mzrt_mutex *mutex, intptr_t seconds, intptr_t nanoseconds) {
  struct timespec timeout;
  timeout.tv_sec  = seconds;
  timeout.tv_nsec = nanoseconds;
  return pthread_cond_timedwait(&cond->cond, &mutex->mutex, &timeout);
}

int mzrt_cond_signal(mzrt_cond *cond) {
  return pthread_cond_signal(&cond->cond);
}

int mzrt_cond_broadcast(mzrt_cond *cond) {
  return pthread_cond_broadcast(&cond->cond);
}

int mzrt_cond_destroy(mzrt_cond *cond) {
  int r;
  r = pthread_cond_destroy(&cond->cond);
  if (!r) free(cond);
  return r;
}

struct mzrt_sema {
  int ready;
  pthread_mutex_t m;
  pthread_cond_t c;
};

int mzrt_sema_create(mzrt_sema **_s, int v)
{
  mzrt_sema *s;
  int err;

  s = (mzrt_sema *)malloc(sizeof(mzrt_sema));
  err = pthread_mutex_init(&s->m, NULL);
  if (err) { 
    free(s); 
    return err; 
  }
  err = pthread_cond_init(&s->c, NULL);
  if (err) { 
    pthread_mutex_destroy(&s->m);
    free(s); 
    return err; 
  }
  s->ready = v;
  *_s = s;

  return 0;
}

int mzrt_sema_wait(mzrt_sema *s)
{
  pthread_mutex_lock(&s->m);
  while (!s->ready) {
    pthread_cond_wait(&s->c, &s->m);
  }
  --s->ready;
  pthread_mutex_unlock(&s->m);

  return 0;
}

int mzrt_sema_trywait(mzrt_sema *s)
{
  int locked = 1;
  pthread_mutex_lock(&s->m);
  if(s->ready) {
    --s->ready;
    locked = 0;
  }
  pthread_mutex_unlock(&s->m);
  return locked;
}

int mzrt_sema_post(mzrt_sema *s)
{
  pthread_mutex_lock(&s->m);
  s->ready++;
  pthread_cond_signal(&s->c);
  pthread_mutex_unlock(&s->m);

  return 0;
}

int mzrt_sema_destroy(mzrt_sema *s)
{
  int r = 0;

  r |= pthread_mutex_destroy(&s->m);
  r |= pthread_cond_destroy(&s->c);

  if (!r) free(s);

  return r;
}

#endif

/* Windows **************************************************************/

#ifdef WIN32

struct mzrt_rwlock {
  HANDLE readEvent;
  HANDLE writeMutex;
  LONG readers;
};

int mzrt_rwlock_create(mzrt_rwlock **lock) {
  *lock = malloc(sizeof(mzrt_rwlock));
  (*lock)->readers = 0;
  /* CreateEvent(LPSECURITY_ATTRIBUTES, manualReset, initiallySignaled, LPCSTR name) */
  if (! ((*lock)->readEvent = CreateEvent(NULL, TRUE, FALSE, NULL)))
    return 0;
  if (! ((*lock)->writeMutex = CreateMutex(NULL, FALSE, NULL)))
    return 0;

  return 1;
}

static int get_win32_os_error() {
  return 0;
}

static int mzrt_rwlock_rdlock_worker(mzrt_rwlock *lock, DWORD millis) {
  DWORD rc = WaitForSingleObject(lock->writeMutex, millis);
  if (rc == WAIT_FAILED || rc == WAIT_TIMEOUT );
    return 0;

  InterlockedIncrement(&lock->readers);

  if (! ResetEvent(lock->readEvent))
    return 0;

  if (!ReleaseMutex(lock->writeMutex))
    return 0;

  return 1;
}

static int mzrt_rwlock_wrlock_worker(mzrt_rwlock *lock, DWORD millis) {
  DWORD rc = WaitForSingleObject(lock->writeMutex, millis);
  if (rc == WAIT_FAILED || rc == WAIT_TIMEOUT );
    return 0;

  if (lock->readers) {
    if (millis) {
      rc = WaitForSingleObject(lock->readEvent, millis);
    }
    else {
      rc = WAIT_TIMEOUT;
    }

    if (rc == WAIT_FAILED || rc == WAIT_TIMEOUT );
      return 0;
  }

  return 1;
}

int mzrt_rwlock_rdlock(mzrt_rwlock *lock) {
  return mzrt_rwlock_rdlock_worker(lock, INFINITE);
}

int mzrt_rwlock_wrlock(mzrt_rwlock *lock) {
  return mzrt_rwlock_wrlock_worker(lock, INFINITE);
}

int mzrt_rwlock_tryrdlock(mzrt_rwlock *lock) {
  return mzrt_rwlock_rdlock_worker(lock, 0);
}

int mzrt_rwlock_trywrlock(mzrt_rwlock *lock) {
  return mzrt_rwlock_wrlock_worker(lock, 0);
}

int mzrt_rwlock_unlock(mzrt_rwlock *lock) {
  DWORD rc = 0;
  if (!ReleaseMutex(lock->writeMutex)) {
    rc = get_win32_os_error();
  }

  if (rc == ERROR_NOT_OWNER) {
    if (lock->readers && !InterlockedDecrement(&lock->readers) && !SetEvent(lock->readEvent)) {
      rc = get_win32_os_error();
    }
    else {
      rc = 0;
    }
  }

  return !rc;
}

int mzrt_rwlock_destroy(mzrt_rwlock *lock) {
  int rc = 1;
  rc &= CloseHandle(lock->readEvent);
  rc &= CloseHandle(lock->writeMutex);
  if (rc) free(lock);
  return !rc;
}

struct mzrt_mutex {
  CRITICAL_SECTION critical_section;
};

int mzrt_mutex_create(mzrt_mutex **mutex) {
  *mutex = malloc(sizeof(mzrt_mutex));
  InitializeCriticalSection(&(*mutex)->critical_section);
  return 0;
}

int mzrt_mutex_lock(mzrt_mutex *mutex) {
  EnterCriticalSection(&mutex->critical_section);
  return 0;
}

int mzrt_mutex_trylock(mzrt_mutex *mutex) {
  /* FIXME: TryEnterCriticalSection() requires NT:
     if (!TryEnterCriticalSection(&mutex->critical_section))
       return 1; */
  return 0;
}

int mzrt_mutex_unlock(mzrt_mutex *mutex) {
  LeaveCriticalSection(&mutex->critical_section);
  return 0;
}

int mzrt_mutex_destroy(mzrt_mutex *mutex) {
  DeleteCriticalSection(&mutex->critical_section);
  free(mutex);
  return 0;
}

struct mzrt_cond {
  int nothing;
};

int mzrt_cond_create(mzrt_cond **cond) {
  return 0;
}

int mzrt_cond_wait(mzrt_cond *cond, mzrt_mutex *mutex) {
  return 0;
}

int mzrt_cond_timedwait(mzrt_cond *cond, mzrt_mutex *mutex, intptr_t secs, intptr_t nsecs) {
  return 0;
}

int mzrt_cond_signal(mzrt_cond *cond) {
  return 0;
}

int mzrt_cond_broadcast(mzrt_cond *cond) {
  return 0;
}

int mzrt_cond_destroy(mzrt_cond *cond) {
  return 0;
}

struct mzrt_sema {
  HANDLE ws;
};

int mzrt_sema_create(mzrt_sema **_s, int v)
{
  mzrt_sema *s;
  HANDLE ws;

  s = (mzrt_sema *)malloc(sizeof(mzrt_sema));
  ws = CreateSemaphore(NULL, v, 32000, NULL);
  s->ws = ws;
  *_s = s;

  return 0;
}

int mzrt_sema_wait(mzrt_sema *s)
{
  WaitForSingleObject(s->ws, INFINITE);
  return 0;
}

int mzrt_sema_trywait(mzrt_sema *s)
{
  return WaitForSingleObject(s->ws, 0);
}

int mzrt_sema_post(mzrt_sema *s)
{
  ReleaseSemaphore(s->ws, 1, NULL);  
  return 0;
}

int mzrt_sema_destroy(mzrt_sema *s)
{
  int r;

  r = CloseHandle(s->ws);
  if (r) free(s);

  return !r;
}

#endif

/************************************************************************/
/************************************************************************/
/************************************************************************/

#ifdef MZ_XFORM
END_XFORM_SUSPEND;
#endif

#endif
