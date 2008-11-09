#ifndef MAD_WORLDTHREAD_H
#define MAD_WORLDTHREAD_H

/// \file worldthread.h
/// \brief Implements Mutex (pthread or spin-lock), Dqueue, Thread, ThreadBase and ThreadPool

#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <utility>
#include <vector>
#include <madness_config.h>
#include <world/madatomic.h>
#include <world/nodefaults.h>
#include <world/worldpapi.h>

namespace madness {
    
    // On the XT the pthread spinlocks seem to have issues ... or perhaps
    // that is me?

#define USE_PTHREAD_MUTEX
    //#define WORLD_MUTEX_ATOMIC
    
    class MutexWaiter {
    private:
        unsigned int count;

        /// Yield for specified number of microseconds
        void yield(int us) {
#ifndef HAVE_CRAYXT
            usleep(us);
#endif
        }
        
    public:
        MutexWaiter() : count(0) {}

        void reset() {count = 0;}

        /// Call inside spin loop to yield processor if waiting a long time

        /// Under Linux with the default scheduler usleep calls
        /// nanosleep that in turn seems to spin for short periods but
        /// otherwise blocks in select with a granularity of the
        /// kernel timer which is circa 1-10ms.  nanosleep() is POSIX
        /// standard. Sched_yield() on the other hand is non-standard
        /// and has undefined resume time and impact on thread
        /// priority.  The in principle nice thing about sched_yield
        /// is that if there is nothing else to run we just keep
        /// running, but this also means we are spinning in the kernel
        /// which can have bigger performance implications.  Hence, to
        /// control the performance impact of yielding we ...
        ///
        /// - Spin for first 10000000 calls --> circa 10ms (at 1GHz polling)
        ///
        /// - For next 1000 sleeps for 1ms --> circa 1s to 10s
        ///
        /// - Subsequently sleep for 10ms
        void wait() {
            const unsigned int nspin = 10000000;
            const unsigned int  nnap = nspin + 1000;
            const int   naptime = 1000;
            const int sleeptime = naptime*10;
            count++;
            if (count < nspin) {
                for (int i=0; i<100; i++) count++;
                count -= 100;
                return;
            }
            else if (count < nnap) {
                yield(naptime);
            }
            else {
                yield(sleeptime);
            }
        }
    };


#ifdef WORLD_MUTEX_ATOMIC
    /// Mutex using spin locks and atomic operations
    
    /// Possibly much *slower* than pthread operations.  Also cannot
    /// use these Mutexes with pthread condition variables.
    class Mutex : NO_DEFAULTS {
    private:
        mutable MADATOMIC_INT flag;
    public:
        /// Make and initialize a mutex ... initial state is unlocked
        Mutex() {
            MADATOMIC_INT_SET(&flag,1L);
        }
        
        /// Try to acquire the mutex ... return true on success, false on failure
        bool try_lock() const {
            if (MADATOMIC_INT_DEC_AND_TEST(&flag)) return true;
            MADATOMIC_INT_INC(&flag);
            return false;
        }
            
        /// Acquire the mutex waiting if necessary
        void lock() const {
            MutexWaiter waiter;
            while (!try_lock()) waiter.wait();
        }
        
        /// Free a mutex owned by this thread
        void unlock() const {
            MADATOMIC_INT_INC(&flag);
        }

        //pthread_mutex_t* ptr() const {throw "there is no pthread_mutex";}
        
        virtual ~Mutex() {};
    };

#else
    
    /// Mutex using pthread mutex operations
    class Mutex {
    private:
#ifdef USE_PTHREAD_MUTEX
        mutable pthread_mutex_t mutex;
#else
        mutable pthread_spinlock_t mutex;
#endif
        /// Copy constructor is forbidden
        Mutex(const Mutex& m) {}
        
        /// Assignment is forbidden
        void operator=(const Mutex& m) {}
        
    public:
        /// Make and initialize a mutex ... initial state is unlocked
        Mutex() {
#ifdef USE_PTHREAD_MUTEX
            pthread_mutex_init(&mutex, 0);
#else
            pthread_spin_init(&mutex, PTHREAD_PROCESS_PRIVATE);
#endif
        }

        /// Try to acquire the mutex ... return true on success, false on failure
        bool try_lock() const {
#ifdef USE_PTHREAD_MUTEX
            return pthread_mutex_trylock(&mutex)==0;
#else
            return pthread_spin_trylock(&mutex)==0;
#endif
        }
        
        /// Acquire the mutex waiting if necessary
        void lock() const {
#ifdef USE_PTHREAD_MUTEX
            if (pthread_mutex_lock(&mutex)) throw "failed acquiring mutex";
#else
            if (pthread_spin_lock(&mutex)) throw "failed acquiring mutex";
#endif
        }
        
        /// Free a mutex owned by this thread
        void unlock() const {
#ifdef USE_PTHREAD_MUTEX
            if (pthread_mutex_unlock(&mutex)) throw "failed releasing mutex";
#else
            if (pthread_spin_unlock(&mutex)) throw "failed releasing mutex";
#endif
        }
        
        /// Return a pointer to the pthread mutex for use by a condition variable
        pthread_mutex_t* ptr() const {
#ifdef USE_PTHREAD_MUTEX
            return &mutex;
#else
            throw "using spinlocks not mutexes";
#endif
        }

        virtual ~Mutex() {
#ifdef USE_PTHREAD_MUTEX
            pthread_mutex_destroy(&mutex);
#else
            pthread_spin_destroy(&mutex);
#endif
        };
    };

 
#endif


    /// Mutex that is applied/released at start/end of a scope

    /// The mutex must provide lock and unlock methods
    template <class mutexT = Mutex>
    class ScopedMutex {
        const mutexT* m;
    public:
        ScopedMutex(const mutexT* m) : m(m) {m->lock();}
        ScopedMutex(const mutexT& m) : m(&m) {m.lock();}
        virtual ~ScopedMutex() {m->unlock();}
    };

//     template <class mutexT>
//     ScopedMutex<mutexT> scoped_mutex(const mutexT& mutex) {return ScopedMutex<mutexT>(mutex);}

//     template <class mutexT>
//     ScopedMutex<mutexT> scoped_mutex(const mutexT* mutex) {return ScopedMutex<mutexT>(mutex);}

    /// Spinlock using pthread spinlock operations
    class Spinlock {
    private:
        mutable pthread_spinlock_t spinlock;

        /// Copy constructor is forbidden
        Spinlock(const Spinlock& m) {}
        
        /// Assignment is forbidden
        void operator=(const Spinlock& m) {}
        
    public:
        /// Make and initialize a spinlock ... initial state is unlocked
        Spinlock() {
            pthread_spin_init(&spinlock, PTHREAD_PROCESS_PRIVATE);
        }

        /// Try to acquire the spinlock ... return true on success, false on failure
        bool try_lock() const {
            return pthread_spin_trylock(&spinlock)==0;
        }
        
        /// Acquire the spinlock waiting if necessary
        void lock() const {
            if (pthread_spin_lock(&spinlock)) throw "failed acquiring spinlock";
        }
        
        /// Free a spinlock owned by this thread
        void unlock() const {
            if (pthread_spin_unlock(&spinlock)) throw "failed releasing spinlock";
        }
        
        virtual ~Spinlock() {
            pthread_spin_destroy(&spinlock);
        };
    };


    class MutexReaderWriter : private Spinlock, NO_DEFAULTS {
        volatile mutable int nreader;
        volatile mutable bool writeflag;
    public:
        static const int NOLOCK=0;
        static const int READLOCK=1;
        static const int WRITELOCK=2;

        MutexReaderWriter() : nreader(0), writeflag(false) 
        {}

        bool try_read_lock() const {
            ScopedMutex<Spinlock> protect(this);
            bool gotit = !writeflag;
            if (gotit) nreader++;
            return gotit;
        }
        
        bool try_write_lock() const {
            ScopedMutex<Spinlock> protect(this);
            bool gotit = (!writeflag) && (nreader==0);
            if (gotit) writeflag = true;
            return gotit;
        }

        bool try_lock(int lockmode) const {
            if (lockmode == READLOCK) {
                return try_read_lock();
            }
            else if (lockmode == WRITELOCK) {
                return try_write_lock();
            }
            else if (lockmode == NOLOCK) {
                return true;
            }
            else {
                throw "MutexReaderWriter: try_lock: invalid lock mode";
            }
        }

        bool try_convert_read_lock_to_write_lock() const {
            ScopedMutex<Spinlock> protect(this);
            bool gotit = (!writeflag) && (nreader==1);
            if (gotit) {
                nreader = 0;
                writeflag = true;
            }
            return gotit;
        }

        void read_lock() const {
            MutexWaiter waiter;
            while (!try_read_lock()) waiter.wait();
        }

        void write_lock() const {
            MutexWaiter waiter;
            while (!try_write_lock()) waiter.wait();
        }

        void lock(int lockmode) const {
            MutexWaiter waiter;
            while (!try_lock(lockmode)) waiter.wait();
        }

        void read_unlock() const {
            ScopedMutex<Spinlock> protect(this);
            nreader--;
        }

        void write_unlock() const {
            // Only a single thread should be setting writeflag but
            // probably still need the mutex just to get memory fence?
            ScopedMutex<Spinlock> protect(this);
            writeflag = false;
        }

        void unlock(int lockmode) const {
            if (lockmode == READLOCK) read_unlock();
            else if (lockmode == WRITELOCK) write_unlock();
            else if (lockmode != NOLOCK) throw "MutexReaderWriter: try_lock: invalid lock mode";            
        }

        /// Converts read to write lock without releasing the read lock

        /// Note that deadlock is guaranteed if two+ threads wait to convert at the same time.
        void convert_read_lock_to_write_lock() const {
            MutexWaiter waiter;
            while (!try_convert_read_lock_to_write_lock()) waiter.wait();
        }

        /// Always succeeds immediately
        void convert_write_lock_to_read_lock() const {
            ScopedMutex<Spinlock> protect(this);
            nreader++;
            writeflag=false;
        }

        virtual ~MutexReaderWriter(){};
    };

    /// Scalable and fair condition variable (spins on local value)
    class ConditionVariable : public Mutex {
    public:
        static const int MAX_NTHREAD = 64;
        mutable volatile int nsig; // No. of outstanding signals
        mutable volatile int front;
        mutable volatile int back;
        mutable volatile bool* volatile q[MAX_NTHREAD]; // Circular buffer of flags

    private:
        void wakeup() {
            // ASSUME we have the lock already when in here
            while (nsig && front != back) {
                nsig--;
                int f = front;
                *q[f] = true;
                q[f] = 0; // To better detect stupidities
                f++;
                if (f >= MAX_NTHREAD) front = 0;
                else front = f;
            }
        }

    public:
        ConditionVariable() : nsig(0), front(0), back(0) {}

        /// You should acquire the mutex before waiting
        void wait() {
            if (nsig) {
                nsig--;
            }
            else if (nsig == 0) {
                // We put a pointer to a thread-local variable at the
                // end of the queue and wait for that value to be set,
                // thus generate no memory traffic while waiting.
                volatile bool myturn = false;
                int b = back;
                q[b] = &myturn;
                b++;
                if (b >= MAX_NTHREAD) back = 0;
                else back = b;
                
                unlock(); // Release lock before blocking
                while (!myturn);
                lock();
            }
            else if (nsig < 0) {
                throw "ConditionVariable: wait: invalid state";
            }
            wakeup();
        }
        
        /// You should acquire the mutex before signalling
        void signal() {
            nsig++;
            wakeup();
        }

        virtual ~ConditionVariable() {}
    };


    /// A scalable and fair mutex (not recursive)

    /// Needs rewriting to use the CV above and do we really
    /// need this if using pthread_mutex .. why not pthread_cv?
    class MutexFair : private Mutex {
    private:
        static const int MAX_NTHREAD = 64;
        mutable volatile bool* volatile q[MAX_NTHREAD]; 
        mutable volatile int n;
        mutable volatile int front;
        mutable volatile int back;

    public:
        MutexFair() : n(0), front(0), back(0) {};

        void lock() const {
            // The mutex controls access to the queue (a circular buffer)
            // On entry, increment n and then ...
            //
            // If (n == 1) the lock is ours. 
            //
            // Otherwise, put oursleves on the back of the queue by
            // appending a pointer to a bool on our own stack.  We
            // then spin waiting for this to become true.  Hence we
            // are not generating memory traffic while waiting.
            //
            // When releasing a lock we must check to see if someone
            // else is waiting ... if they are then update front/back,
            // release the lock and set their flag
            volatile bool myturn = false;
            Mutex::lock();
            if (n < 0 || n >= MAX_NTHREAD) throw "MutexFair: lock: invalid state?";
            n++;
            if (n == 1) {
                if (front != back) throw "MutexFair: lock: invalid state (2) ?";
                myturn = true;
            }
            else {
                back++;
                if (back >= MAX_NTHREAD) back = 0;
                q[back] = &myturn;
            }
            Mutex::unlock();

            if (!myturn) {
                //std::cout << "WAITING IN FAIR LOCK " << (void *) &myturn << std::endl;
                MutexWaiter waiter;
                while (!myturn) waiter.wait();
                //std::cout << "WOKENUP AFTER WAITING IN FAIR LOCK " << (void *) &myturn << std::endl;
            }

            return;
        }

        void unlock() const {
            volatile bool* p = 0;
            Mutex::lock();
            if (n < 1 || n > MAX_NTHREAD) throw "MutexFair: unlock: invalid state?";
            n--;
            if (n > 0) {
                front++;
                if (front >= MAX_NTHREAD) front = 0;
                p = q[front];
                //std::cout << "IN MUTEXFAIRUNLOCK " << n << " " << front << " " << back << " " << (void *) p << std::endl;
            }
            else {
                if (front != back) throw "MutexFair: unlock: invalid state (2) ?";
            }
            Mutex::unlock();
            if (p) {
                //std::cout << "ABOUT TO UNLOCK " << (void *)(p) << std::endl;
                *p = true;
            }
        }

        /// Makes little sense to spin outside a fair lock ... but if you wanna ...
        bool try_lock() const {
            bool got_lock;

            Mutex::lock();
            got_lock = (n == 0);
            if (got_lock) n++;
            Mutex::unlock();

            return got_lock;
        }
                
    };


    /// Attempt to acquire two locks without blocking while holding either one

    /// The code will first attempt to acquire mutex m1 and if successful
    /// will then attempt to acquire mutex m2.  
    inline bool try_two_locks(const Mutex& m1, const Mutex& m2) {
        if (!m1.try_lock()) return false;
        if (m2.try_lock()) return true;
        m1.unlock();
        return false;
    }


    /// Simple wrapper for Pthread condition variable with its own mutex

    /// Use this when you need to block without consuming cycles.
    /// Scheduling granularity is at the level of kernel ticks.
    class PthreadConditionVariable : NO_DEFAULTS {
    private:
        pthread_cond_t cv;
        pthread_mutex_t mutex;

    public:
        PthreadConditionVariable() {
            pthread_cond_init(&cv, NULL);
            pthread_mutex_init(&mutex, 0);
        }            

        pthread_mutex_t& get_pthread_mutex() {return mutex;}

        void lock() {
            if (pthread_mutex_lock(&mutex)) throw "ConditionVariable: acquiring mutex";
        }

        void unlock() {
            if (pthread_mutex_unlock(&mutex)) throw "ConditionVariable: releasing mutex";
        }

        /// You should have acquired the mutex before entering here
        void wait() {
            pthread_cond_wait(&cv,&mutex);
        }

        void signal() {
            if (pthread_cond_signal(&cv)) throw "ConditionalVariable: signalling failed";
        }

        virtual ~PthreadConditionVariable() {
            pthread_mutex_destroy(&mutex);
            pthread_cond_destroy(&cv);
        }

    };


    struct DQStats { // Dilly bar, blizzard, ...
        uint64_t nmax;          //< Lifetime max. entries in the queue
        uint64_t npush_back;    //< #calls to push_back
        uint64_t npush_front;   //< #calls to push_front
        uint64_t npop_back;     //< #calls to pop_back
        uint64_t npop_front;    //< #calls to pop_front
        uint64_t ngrow;         //< #calls to grow

        DQStats() 
            : nmax(0), npush_back(0), npush_front(0), npop_back(0), npop_front(0), ngrow(0)
        {}
    };


    /// A thread safe, fast but simple doubled-ended queue.  You can push or pop at either end ... that's it.

    /// Since the point is speed, the implementation is a circular
    /// buffer rather than a linked list so as to avoid the new/del
    /// overhead.  It will grow as needed, but presently will not
    /// shrink.  Had to modify STL API to make things thread safe.
    template <typename T>
    class DQueue : private ConditionVariable {
        volatile size_t sz;              ///< Current capacity
        volatile T* volatile buf;        ///< Actual buffer
        volatile int _front;  ///< Index of element at front of buffer
        volatile int _back;    ///< Index of element at back of buffer
        volatile size_t n;      ///< Number of elements in the buffer
        DQStats stats;

        void grow() {
            // ASSUME WE ALREADY HAVE THE MUTEX WHEN IN HERE
            stats.ngrow++;
            if (sz != n) throw "assertion failure in dqueue::grow";
            size_t oldsz = sz;
            if (sz < 32768) 
                sz = 65536;
            else if (sz <= 1048576) 
                sz *= 2;
            else 
                sz += 1048576;
            volatile T* volatile nbuf = new T[sz];
            int lo = sz/2 - oldsz/2;
            for (int i=_front; i<int(oldsz); i++,lo++) {
                nbuf[lo] = buf[i];
            }
            if (_front > 0) {
                for (int i=0; i<=_back; i++,lo++) {
                    nbuf[lo] = buf[i];
                }
            }
            _front = sz/2 - oldsz/2;
            _back = _front + n - 1;
            buf = nbuf;
            sanity_check();
        }

        void sanity_check() const {
            // ASSUME WE ALREADY HAVE THE MUTEX WHEN IN HERE
            int num = _back - _front + 1;
            if (num < 0) num += sz;
            if (num==int(sz) && n==0) num=0;
            if (num==0 && n==sz) num=sz;
            //if (long(n) != num) print("size",sz,"front",_front,"back",_back,"n",n,"num",num);
            if (long(n) != num) throw "assertion failure in dqueue::sanity";
        }

    public:
        DQueue(size_t hint=32768) 
            : sz(hint>2 ? hint : 2)
            , buf(new T[sz])
            , _front(sz/2)
            , _back(_front-1)
            , n(0)
        {}

        virtual ~DQueue() {
            delete buf;
        }

        /// Insert value at front of queue
        void push_front(const T& value) {
            madness::ScopedMutex<ConditionVariable> obolus(this);
            stats.npush_front++;
            //sanity_check();
            if (n == sz) grow();
            _front--;
            if (_front < 0) _front = sz-1;
            buf[_front] = value;
            n++;
            if (n > stats.nmax) stats.nmax = n;
            signal();
        }

        /// Insert element at back of queue
        void push_back(const T& value) {
            madness::ScopedMutex<ConditionVariable> obolus(this);
            stats.npush_back++;
            //sanity_check();
            if (n == sz) grow();
            _back++;
            if (_back >= int(sz)) _back = 0;
            buf[_back] = value;
            n++;
            if (n > stats.nmax) stats.nmax = n;
            signal();
        }

        /// Pop value off the front of queue
        std::pair<T,bool> pop_front(bool wait) {
            madness::ScopedMutex<ConditionVariable> obolus(this);
            stats.npop_front++;
            if (wait) ConditionVariable::wait();
            bool status=true; 
            T result = T();
            if (n) {
                //sanity_check();
                n--;
                result = buf[_front];
                _front++;
                if (_front >= long(sz)) _front = 0;
            }
            else {
                status = false;
            }
            return std::pair<T,bool>(result,status);
        }


        /// Pop value off the back of queue
        std::pair<T,bool> pop_back(bool wait=true) {
            madness::ScopedMutex<ConditionVariable> obolus(this);
            stats.npop_back++;
            if (wait) ConditionVariable::wait();
            bool status=true; 
            T result;
            if (n) {
                //sanity_check();
                n--;
                result = buf[_back];
                _back--;
                if (_back<0) _back = sz-1;
            }
            else {
                status = false;
            }
            return std::pair<T,bool>(result,status);
        }

        size_t size() const {
            return n;
        }
        
        bool empty() const {
            return n==0;
        }

        const DQStats& get_stats() const {
            return stats;
        }
    };

    class ThreadPool;           // Forward decl.
    class WorldTaskQueue;

    /// Simplified thread wrapper to hide pthread complexity

    /// If the thread is using any of the object state you cannot
    /// delete the object until the thread has terminated.
    ///
    /// The cleanest solution is to put the object on the heap and
    /// have the run method "delete this" at its end.
    class ThreadBase {
        friend class ThreadPool;
        static bool bind[3];
        static int cpulo[3];
        static int cpuhi[3];

        static void* main(void* self) {
#ifdef HAVE_PAPI
            begin_papi_measurement();
#endif

            ((ThreadBase*)(self))->run(); 

#ifdef HAVE_PAPI
            end_papi_measurement();
#endif
            return 0;
        }

        int pool_num; ///< Stores index of thread in pool or -1
        pthread_t id; 

        void set_pool_thread_index(int i) {
            pool_num = i;
        }

    public:
        
        /// Default constructor ... must invoke \c start() to actually begin the thread.
        ThreadBase() : pool_num(-1) {};

        virtual ~ThreadBase() {};
        
        /// You implement this to do useful work
        virtual void run() = 0;
        
        /// Start the thread running
        void start() {
            pthread_attr_t attr;
            // Want detached thread with kernel scheduling so can use multiple cpus
            // RJH ... why not checking success/failure????
            pthread_attr_init(&attr);    
            pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
            pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM); 
            if (pthread_create(&id, &attr, &ThreadBase::main, (void *) this))
                throw "failed creating thread";
            
            pthread_attr_destroy(&attr);
        }
        
        /// A thread can call this to terminate its execution
        static void exit() {pthread_exit(0);}
        
        /// Get the pthread id of this thread (if running)
        const pthread_t& get_id() const {return id;}

        /// Get index of thread in ThreadPool (0,...,nthread-1) or -1 if not in ThreadPool
        int get_pool_thread_index() const {return pool_num;}
        
        /// Cancel this thread
        int cancel() const {return pthread_cancel(get_id());}

        /// Specify the affinity pattern or how to bind threads to cpus
        static void set_affinity_pattern(const bool bind[3], const int cpu[3]) {
            memcpy(ThreadBase::bind, bind, 3*sizeof(bool));
            memcpy(ThreadBase::cpulo, cpu, 3*sizeof(int));

            int ncpu = sysconf(_SC_NPROCESSORS_CONF);
            if (ncpu <= 0) throw "ThreadBase: set_affinity_pattern: sysconf(_SC_NPROCESSORS_CONF)";

            // impose sanity and compute cpuhi
            for (int i=0; i<3; i++) {
                if (cpulo[i] < 0) cpulo[i] = 0;
                if (cpulo[i] >= ncpu) cpulo[i] = ncpu-1;

                if (i<2 && bind[i]) cpuhi[i] = cpulo[i];
                else cpuhi[i] = ncpu-1;

                //std::cout << "PATTERN " << i << " " << bind[i] << " " << cpulo[i] << " " << cpuhi[i] << std::endl;
            }
        }

        static void set_affinity(int logical_id, int ind=-1) {
            return;
            if (logical_id < 0 || logical_id > 2) {
                std::cout << "ThreadBase: set_affinity: logical_id bad?" << std::endl;
                return;
            }

            if (!bind[logical_id]) return;
                
            int ncpu = sysconf(_SC_NPROCESSORS_CONF);
            if (ncpu <= 0) {
                std::cout << "ThreadBase: set_affinity_pattern: sysconf(_SC_NPROCESSORS_CONF)" << std::endl;
                return;
            }

            // If binding the main or rmi threads the cpu id is a specific cpu.
            //
            // If binding a pool thread, the cpuid is the lowest cpu
            // to be used.
            //
            // If floating any thread it is floated from the cpuid to ncpu-1

            int lo=cpulo[logical_id], hi=cpuhi[logical_id];

            if (logical_id == 2) {
                if (ind < 0) {
                    std::cout << "ThreadBase: set_affinity: pool thread index bad?" << std::endl;
                    return;
                }
                if (bind[2]) {
                    int nnn = hi-lo+1;
                    lo += (ind % nnn);
                    hi = lo;
                }
            }

            //std::cout << "BINDING THREAD: id " << logical_id << " ind " << ind << " lo " << lo << " hi " << hi << " ncpu " << ncpu << std::endl;
            
            cpu_set_t mask;
            CPU_ZERO(&mask);
            for (int i=lo; i<=hi; i++) CPU_SET(i,&mask);
            if(sched_setaffinity( 0, sizeof(mask), &mask ) == -1 ) {
                perror("system error message");
                std::cout << "ThreadBase: set_affinity: Could not set cpu Affinity" << std::endl;

            }
        }
    };
    
    
    /// Simplified thread wrapper to hide pthread complexity
    class Thread : public ThreadBase {
        void* (*f)(void *); 
        void* args;
        
        void run() {f(args);}
    public:
        /// Default constructor ... must invoke \c start() to actually begin the thread.
        Thread() : f(0), args(0) {};
        
        /// Create a thread and start it running f(args)
        Thread(void* (*f) (void *), void* args=0) 
            : f(f), args(args) 
        {
            ThreadBase::start();
        }
        
        void start(void* (*f) (void *), void* args=0) {
            this->f = f;
            this->args = args;
            ThreadBase::start();
        }

        virtual ~Thread() {}
    };
    
    
    /// Contains attributes of a task
    
    /// \c generator : Setting this hints that a task will produce
    /// additional tasks and is used by the scheduler to
    /// increase/throttle parallelism. The default is false.
    ///
    /// \c stealable : Setting this indicates that a task may be
    /// migrated to another process for dynamic load balancing.  The
    /// default value is false.
    ///
    /// \c highpriority : indicates a high priority task.
    class TaskAttributes {
        unsigned long flags;
        static const unsigned long one = 1ul;
    public:
        static const unsigned long GENERATOR    = one;
        static const unsigned long STEALABLE    = one<<1;
        static const unsigned long HIGHPRIORITY = one<<2;

        TaskAttributes(unsigned long flags = 0) : flags(flags) {}

        bool is_generator() const {return flags&GENERATOR;}

        bool is_stealable() const {return flags&STEALABLE;}

        bool is_high_priority() const {return flags&HIGHPRIORITY;}

        void set_generator (bool generator_hint) {
            if (generator_hint) flags |= GENERATOR;
            else flags &= ~GENERATOR;
        }

        void set_stealable (bool stealable) {
            if (stealable) flags |= STEALABLE;
            else flags &= ~STEALABLE;
        }

        void set_highpriority (bool hipri) {
            if (hipri) flags |= HIGHPRIORITY;
            else flags &= ~HIGHPRIORITY;
        }

        template <typename Archive>
        void serialize(Archive& ar) {
            ar & flags;
        }

        static TaskAttributes generator() {return TaskAttributes(GENERATOR);}

        static TaskAttributes hipri() {return TaskAttributes(HIGHPRIORITY);}
    };


    class PoolTaskInterface : public TaskAttributes {
    public:
        PoolTaskInterface() {}

        explicit PoolTaskInterface(const TaskAttributes& attr)
            : TaskAttributes(attr) {}

        virtual void run() = 0;

        virtual ~PoolTaskInterface() {}
    };


    /// A singleton pool of threads for dynamic execution of tasks.

    /// YOU MUST INSTANTIATE THE POOL WHILE RUNNING WITH JUST ONE THREAD
    class ThreadPool {
    private:
        friend class WorldTaskQueue;
        Thread *threads;              ///< Array of threads
        DQueue<PoolTaskInterface*> queue; ///< Queue of tasks
        int nthreads;		  ///< No. of threads
        volatile bool finish;              ///< Set to true when time to stop
        MADATOMIC_INT nfinished;

        static ThreadPool* instance_ptr;

        /// The constructor is private to enforce the singleton model
        ThreadPool(int nthread=-1) : nthreads(nthread), finish(false) {
            MADATOMIC_INT_SET(&nfinished,0);
            instance_ptr = this;
            if (nthreads < 0) nthreads = default_nthread();
            //std::cout << "POOL " << nthreads << std::endl;

            try {
                if (nthreads > 0) threads = new Thread[nthreads];
            else threads = 0;
            }
            catch (...) {
                throw "memory allocation failed";
            }

            for (int i=0; i<nthreads; i++) {
                threads[i].set_pool_thread_index(i);
                threads[i].start(pool_thread_main, (void *) (threads+i));
            }
        }

        ThreadPool(const ThreadPool&);           // Verboten
        void operator=(const ThreadPool&);       // Verboten

        /// Get number of threads from the environment
        int default_nthread() {
            int nthread;
            char *cnthread = getenv("POOL_NTHREAD");

            if (cnthread) {
                if (sscanf(cnthread, "%d", &nthread) != 1) 
                    throw "POOL_NTHREAD is not an integer";
            }
            else {
                nthread = sysconf(_SC_NPROCESSORS_CONF);
                if (nthread < 2) nthread = 2;
                nthread = nthread - 1; // One less than # physical processors
            }
            return nthread;
        }

        /// Run next task ... returns true if one was run ... blocks if wait is true
        bool run_task(bool wait) {
            std::pair<PoolTaskInterface*,bool> t = queue.pop_front(wait);
            if (t.second) {
                t.first->run();          // What we are here to do
                delete t.first;
            }
            return t.second;
        }

        void thread_main(Thread* thread) {
            thread->set_affinity(2, thread->get_pool_thread_index());
            while (!finish) 
                run_task(true);
            MADATOMIC_INT_INC(&nfinished);
            std::cout << "THREADENDING\n";
        }

        /// Forwards thread to bound member function
        static void* pool_thread_main(void *v) {
            instance()->thread_main((Thread*)(v));
            return 0;
        }


        /// Return a pointer to the only instance constructing as necessary
        static ThreadPool* instance(int nthread=-1) {
            if (!instance_ptr) instance_ptr = new ThreadPool(nthread);
            return instance_ptr;
        }

        class PoolTaskNull : public PoolTaskInterface {
            void run() {
                std::cout << "NULL RUNNING " << std::endl;
            };
        };

    public:
        /// Please invoke while in single threaded environment
        static void begin(int nthread=-1) {instance(nthread);}

        static void end() {
            instance()->finish = true;
            std::cout << "ENDING THREADS" << std::endl;
            for (int i=0; i<instance()->nthreads; i++) {
                std::cout << "ADDING PTN\n";
                add(new PoolTaskNull);
            }
            while (MADATOMIC_INT_GET(&instance()->nfinished) != instance()->nthreads);
            std::cout << "DONE ENDING THREADS" << std::endl;
        }

        /// Add a new task to the pool
        static void add(PoolTaskInterface* task) {
            if (task->is_high_priority()) {
                instance()->queue.push_front(task);
            }
            else {
                instance()->queue.push_back(task);
            }
        }

        /// Add a vector of tasks to the pool
        static void add(const std::vector<PoolTaskInterface*>& tasks) {
            typedef std::vector<PoolTaskInterface*>::const_iterator iteratorT;
            for (iteratorT it=tasks.begin(); it!=tasks.end(); ++it) {
                add(*it);
            }
        }



        /// An otherwise idle thread can all this to run a task

        /// Returns true if one was run
        static bool run_task() {
            return instance()->run_task(false);
        }


        /// Returns number of threads in the pool
        static size_t size() {
            return instance()->nthreads;
        }


        /// Returns queue statistics
        static const DQStats& get_stats() {
            return instance()->queue.get_stats();
        }
        

        ~ThreadPool() {};
    };

    /// Dummy class a la Intel TBB used to distinguish splitting constructor
    class Split{};
    
    /// Range vaguely a la Intel TBB encapsulates STL-like start and end iterators with chunksize
    template <typename iteratorT>
    class Range {
        long n;
        iteratorT start;
        iteratorT finish;
        int chunksize;
    public:
        typedef iteratorT iterator;

        /// Makes the range [start,finish) ... cost is O(n) due to dumb, linear counting of items
        
        /// The motivated reader should look at the Intel TBB range,
        /// partitioner, split, concepts, etc..
        ///
        /// Default chunksize is to make 10 tasks per thread to
        /// facilitate dynamic load balancing.  
        Range(const iterator& start, const iterator& finish, int chunksize=-1) 
            : n(0), start(start), finish(finish), chunksize(chunksize)
        {
            for (iterator it=start; it!=finish; ++it) n++;
            if (chunksize == -1) chunksize = n / (10*ThreadPool::size());
            if (chunksize < 1) chunksize = 1;
        }
        
        /// Copy constructor ... cost is O(1)
        Range(const Range& r) 
            : n(r.n), start(r.start), finish(r.finish), chunksize(r.chunksize)
        {}
        
        /// Splits range between new and old (r) objects ... cost is O(n/2)
        
        /// Presently only bisection down to given chunksize and
        /// executes iterator circa Nlog(N) times so it had better be cheap
        /// compared to the operation being performed.
        Range(Range& r, const Split& split) 
            : n(0), start(r.start), finish(), chunksize(r.chunksize)
        {
            if (r.n > chunksize) {
                long nhalf = n/2;
                while (nhalf--) {
                    n++;
                    r.n--;
                    ++r.start;
                }
            }
            finish = r.start;
        }
        
        /// Returns number of items in the range (cost is O(1))
        size_t size() const {return n;}
        
        /// Returns true if size=0
        bool empty() const {return n==0;}

        const iterator& begin() const {return start;}

        const iterator& end() const {return finish;}
    };

}
    

#endif
