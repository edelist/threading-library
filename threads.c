#include <pthread.h>
#include <setjmp.h>
#include <sys/time.h>
#include "ec440threads.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <semaphore.h>

// define constants and registers
#define THREAD_MAX 128
#define STACK_SIZE 32767
#define JB_RBX 0
#define JB_RBP 1
#define JB_R12 2
#define JB_R13 3
#define JB_R14 4
#define JB_R15 5
#define JB_RSP 6
#define JB_PC 7

// define thread states
typedef enum {
        READY,
        RUNNING,
        EXIT,
        EMPTY,
        BLOCKED
} ThreadState;

// define TCB
struct thread {
        pthread_t id; 
        void* stack;
        jmp_buf context;
        void *(*start_routine) (void *); 
        void* arg;
        ThreadState state;
        void* exit_status; // store exit status
        int waiting_thread; // ID of thread waiting on this thread
};

// define thread pool
struct thread threadPool[THREAD_MAX];

// define global variables
pthread_t gCurrent = 0; // current thread
struct sigaction sa; 
static int gLockAcquired = 0; // 0 is no lock, 1 is lock acquired
static pthread_t gLockOwner; // ID of thread that azquired lock


// thread scheduler
void schedule() {

        // if current thread is RUNNING, set to READY
        if (threadPool[gCurrent].state == RUNNING) threadPool[gCurrent].state = READY;

                pthread_t next_tid = gCurrent;
                int checkedThreads = 0; // counter for checked threads

                // find next READY thread
                do {
                        next_tid = (next_tid + 1) % THREAD_MAX; // circular scheduling
                        checkedThreads++;

                        // break if no threads are READY
                        if (checkedThreads >= THREAD_MAX) return;
                } while (threadPool[next_tid].state != READY);

                // if current thread has not exited, save context
                int save = 0;
                if (threadPool[gCurrent].state != EXIT) {
                        save = setjmp(threadPool[gCurrent].context);
                }

                // if current conext was not saved, switch context
                if (!save) {
                        gCurrent = next_tid;
                        threadPool[gCurrent].state = RUNNING;
                        longjmp(threadPool[gCurrent].context, 1);
                }
}

// locking: set lock flag and register current thread as lock ownder
void lock() {

        // block SIGALRM
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_BLOCK, &set, NULL);

        gLockAcquired = 1;
        gLockOwner = gCurrent;
}

void unlock() {
        assert(gLockAcquired && gCurrent == gLockOwner); // assert current thread owns lock

        // clear lock
        gLockAcquired = 0;
        gLockOwner = -1;

        // unblock SIGALRM
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
}


// alarm signal handler
void alarm_handler(int signum) {
        if (signum == SIGALRM) {
                schedule();
        }
}

static void set_alarm() {

        // initialize all thread to EMPTY and set thread IDs
        int i;
        for (i=0; i<THREAD_MAX; i++) {
                threadPool[i].state = EMPTY;
                threadPool[i].id = i;
        }

        // generate timer struct
        struct itimerval timer;

        // set up signal handler
        memset(&sa, 0, sizeof(sa)); // clear sa struct
        sigemptyset(&sa.sa_mask); // clear any blocked signals
        sa.sa_handler = &alarm_handler; // map signal handler
        sa.sa_flags = SA_NODEFER; // allow signal to be received when handler is executing
        if (sigaction(SIGALRM, &sa, NULL)==-1) { // register handler
                perror("ERROR: Unable to catch SIGALARM.");
                exit(EXIT_FAILURE);
        }

        // set 50ms timer
        timer.it_value.tv_sec = 0;
        timer.it_value.tv_usec = 50000;
        timer.it_interval = timer.it_value;
        if (setitimer(ITIMER_REAL, &timer, NULL)==-1) { // start timer
                perror("ERROR: Failed to set itimer.");
                exit(EXIT_FAILURE);
        }
}

// creates threads
int pthread_create (
        pthread_t *thread,
        const pthread_attr_t *attr,
        void *(*start_routine) (void *),
        void *arg
) {
        attr = NULL; // set to NULL for now
        static int init = 1;
        int main_thread = 0; // flag for main thread

        // check if first call - set alarm and main thread 0
        if (init) {
                set_alarm();
                init = 0;
                threadPool[0].state = READY;
                main_thread = setjmp(threadPool[0].context);
        }



        // find next thread id and error check  
        if (!main_thread) {

                // look for next EMPTY thread to create
                pthread_t gNextThreadId = 1;
                while (threadPool[gNextThreadId].state != EMPTY && gNextThreadId < THREAD_MAX) {
                        gNextThreadId++;
                }

                // error check thread count
                if (gNextThreadId > THREAD_MAX) {
                        perror("ERROR: Exceeded max number of threads.");
                        exit(EXIT_FAILURE);
                }

                // return new thread ID
                *thread = gNextThreadId;

                // save current state
                setjmp(threadPool[gNextThreadId].context);

                // setup jump_buf context
                threadPool[gNextThreadId].context[0].__jmpbuf[JB_PC] = ptr_mangle((unsigned long int)start_thunk); // mangle new PC
                threadPool[gNextThreadId].context[0].__jmpbuf[JB_R12] = (unsigned long int) start_routine;
                threadPool[gNextThreadId].context[0].__jmpbuf[JB_R13] = (long) arg;

                // allocate new stack and error check
                threadPool[gNextThreadId].stack = malloc(STACK_SIZE);
                if (!threadPool[gNextThreadId].stack) {
                        perror("ERROR: Failed to allocate stack memory.");
                        exit(EXIT_FAILURE);
                }

                // set new stack pointer
                void* stackptr = threadPool[gNextThreadId].stack;

                // push address of pthread_exit onto stack
                stackptr = (char*)stackptr + STACK_SIZE - sizeof(void*); // cast to char ptr, move ptr to top of stack, make space for a pointer to pthread_exit()
                *((void**)stackptr) = pthread_exit; // dereference double pointer once, set to point to address of pthread_exit()

                // push stack pointer to jmp_buf context
                threadPool[gNextThreadId].context[0].__jmpbuf[JB_RSP] = ptr_mangle((unsigned long int)stackptr);

                // initialize waiting_threads to -1
                int i;
                for (i=0; i<THREAD_MAX; i++) {
                        threadPool[gNextThreadId].waiting_thread = -1;
                }

                // set new thread ID and set to READY
                threadPool[gNextThreadId].state = READY;
                threadPool[gNextThreadId].id = gNextThreadId;

                // call to scheduler
                schedule();
        }
        return 0;
}

int pthread_join(pthread_t thread, void** value_ptr) {
        if (thread >= THREAD_MAX || threadPool[thread].state == EMPTY) {
                return -1; // invalid ID or thread not created
        }

        lock();
                        if (threadPool[thread].waiting_thread == -1) {
                                threadPool[thread].waiting_thread = gCurrent;
                        } else {
                                unlock();
                                return -1;
                        }


                // check if target thread has exited - block current thread and schedule
                while (threadPool[thread].state != EXIT) {
                        threadPool[gCurrent].state = BLOCKED;
                        unlock();
                        schedule();
                        lock();
                }

        // retrieve exit value when current thread resumes
        if (value_ptr != NULL) {
                *value_ptr = threadPool[thread].exit_status;
        }
        return 0;
}

int pthread_join(pthread_t thread, void** value_ptr) {
        if (thread >= THREAD_MAX || threadPool[thread].state == EMPTY) {
                return -1; // invalid ID or thread not created
        }

        lock();
                        if (threadPool[thread].waiting_thread == -1) {
                                threadPool[thread].waiting_thread = gCurrent;
                        } else {
                                unlock();
                                return -1;
                        }


                // check if target thread has exited - block current thread and schedule
                while (threadPool[thread].state != EXIT) {
                        threadPool[gCurrent].state = BLOCKED;
                        unlock();
                        schedule();
                        lock();
                }

        // retrieve exit value when current thread resumes
        if (value_ptr != NULL) {
                *value_ptr = threadPool[thread].exit_status;
        }
        return 0;

}

void pthread_exit_wrapper() {
        unsigned long int res;
        asm("movq %%rax, %0\n":"=r"(res));
        pthread_exit((void*) res);
}



// exit function
void pthread_exit(void *value_ptr) {
        int had_lock = 0;
        // unlock
        if (gLockAcquired && gCurrent == gLockOwner) {
                unlock();
                had_lock = 1;
        }

        lock();

        // set exit value for current thread
        threadPool[gCurrent].exit_status = value_ptr;

        // if another thread is waiting, set it to READY
                if (threadPool[gCurrent].waiting_thread != -1) {
                        //pthread_t waiting = threadPool[gCurrent].waiting_thread;
                        if (threadPool[threadPool[gCurrent].waiting_thread].state == BLOCKED) {
                                threadPool[threadPool[gCurrent].waiting_thread].state = READY;
                        }
                        threadPool[gCurrent].waiting_thread = -1; // reset
                }

        // set current thread to EXIT
        threadPool[gCurrent].state = EXIT;

        // if cirrent rhead has stack memory, free it
        if (threadPool[gCurrent].stack) {
                free(threadPool[gCurrent].stack);
                threadPool[gCurrent].stack = NULL; // set ptr to NULL
        }

        if (had_lock) unlock();
        schedule(); // call scheduler to run another thread

        // should NOT reach this point
        perror("ERROR: Scheduling error from pthread_exit().");
        exit(EXIT_FAILURE);
}


// return thread ID of calling thread
pthread_t pthread_self(void) {
        return gCurrent;
}

// defining new semaphore type
typedef struct my_semaphore {
        int value;
        int is_initialized;
        pthread_t waiting_threads[THREAD_MAX]; // waiting queue
        int front;
        int rear;
} my_semaphore_t;

int sem_init(sem_t* sem, int pshared, unsigned value) {
        if (pshared != 0) {
                perror("ERROR: pshared value is not 0.");
                return -1; // error
        }

        // create and initialize semaphore structure
        my_semaphore_t* my_sem = (my_semaphore_t*)malloc(sizeof(my_semaphore_t));
        if (my_sem==NULL) {
                perror("ERROR: Failed memory allocation for sem_init.");
                return -1; // error
        }

        // create and initialize semaphore structure
        my_semaphore_t* my_sem = (my_semaphore_t*)malloc(sizeof(my_semaphore_t));
        if (my_sem==NULL) {
                perror("ERROR: Failed memory allocation for sem_init.");
                return -1; // error
        }

        my_sem->value = value;
        my_sem->is_initialized = 1;
        my_sem->front = 0;
        my_sem->rear = 0;

        // store reference to semaphore
        sem->__align = (long) my_sem;

        return 0;
}

int sem_wait(sem_t* sem) {
        lock();
        my_semaphore_t* my_sem = (my_semaphore_t*) sem->__align;

        // check if all resources are being used
        while (my_sem->value <= 0) {
                // add current thread to waiting queue
                my_sem->waiting_threads[my_sem->rear++] = gCurrent;
                unlock();
                schedule(); // yield CPU to allow other threads to run
                lock();
        }

        // use resource - decrement semaphore
        my_sem->value--;

        unlock();
        return 0;
}

int sem_post(sem_t* sem) {
        lock();
        my_semaphore_t* my_sem = (my_semaphore_t*) sem->__align;
        my_sem->value++; // resource available

        // check if threads are waiting for resource
        if (my_sem->front < my_sem->rear) {
                // wake up first waiting thread
                pthread_t next_thread = my_sem->waiting_threads[my_sem->front++];
                threadPool[next_thread].state = READY;
        }

        unlock();
        return 0;
}

int sem_destroy(sem_t* sem) {
        my_semaphore_t* my_sem = (my_semaphore_t*) sem->__align;
        if (my_sem->front < my_sem-> rear) {
                perror("ERROR: Attempted to destroy active semaphore.");
                return -1;
        }

        free(my_sem);
        return 0;
}