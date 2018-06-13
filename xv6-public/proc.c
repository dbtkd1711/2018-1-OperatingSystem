#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
    struct spinlock lock;
    struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
uint boosting_ticks = 0;
double MLFQ_tickets = 100;  // Tickets for MLFQ = MLFQ's cpu shares
double stride_tickets = 0;  // Sum of all stride mode processes' tickets = strides' cpu shares

double MLFQ_stride = 0;

double MLFQ_pass = 0;
double stride_pass = 0;

extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
    initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
    return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
    int apicid, i;

    if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");

    apicid = lapicid();
    // APIC IDs are not guaranteed to be contiguous. Maybe we should have
    // a reverse map, or reserve a register to store &cpus[i].
    for (i = 0; i < ncpu; ++i) {
        if (cpus[i].apicid == apicid)
        return &cpus[i];
    }
    panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
    struct cpu *c;
    struct proc *p;
    pushcli();
    c = mycpu();
    p = c->proc;
    popcli();
    return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
    struct proc *p;
    char *sp;

    acquire(&ptable.lock);

    // Reset all processes' pass as 0
    for(p=ptable.proc ; p<&ptable.proc[NPROC] ; p++)
        p->pass = 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if(p->state == UNUSED)
        goto found;

    release(&ptable.lock);
    return 0;

found:
    p->state = EMBRYO;
    p->pid = nextpid++;
    p->mthread = p;
    // Initialize p as highest prioriy mlfq mode process
    p->mlfq_mode = 1;
    p->prior_level = 0;
    p->quantum = 0;
    p->allotment = 0;
    p->total_tickets = 0;
    p->tickets = 0;
    p->stride = 0;
    p->pass = 0;
    p->is_thread = 0;
    p->tid = 0;
    p->num_thread = 0;

    release(&ptable.lock);

    // Allocate kernel stack.
    if((p->kstack = kalloc()) == 0){
        p->state = UNUSED;
        return 0;
    }
    sp = p->kstack + KSTACKSIZE;

    // Leave room for trap frame.
    sp -= sizeof *p->tf;
    p->tf = (struct trapframe*)sp;

    // Set up new context to start executing at forkret,
    // which returns to trapret.
    sp -= 4;
    *(uint*)sp = (uint)trapret;

    sp -= sizeof *p->context;
    p->context = (struct context*)sp;
    memset(p->context, 0, sizeof *p->context);
    p->context->eip = (uint)forkret;

    return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
    struct proc *p;
    extern char _binary_initcode_start[], _binary_initcode_size[];

    p = allocproc();

    initproc = p;
    if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
    inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
    p->sz = PGSIZE;
    memset(p->tf, 0, sizeof(*p->tf));
    p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
    p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
    p->tf->es = p->tf->ds;
    p->tf->ss = p->tf->ds;
    p->tf->eflags = FL_IF;
    p->tf->esp = PGSIZE;
    p->tf->eip = 0;  // beginning of initcode.S

    safestrcpy(p->name, "initcode", sizeof(p->name));
    p->cwd = namei("/");

    // this assignment to p->state lets other cores
    // run this process. the acquire forces the above
    // writes to be visible, and the lock is also needed
    // because the assignment might not be atomic.
    acquire(&ptable.lock);
    p->state = RUNNABLE;
    release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
    uint sz;
    struct proc *p;
    struct proc *curproc = myproc();

    sz = curproc->sz;
    if(n > 0){
        if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
        return -1;
    } else if(n < 0){
        if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
        return -1;
    }

    // All mthread and wthreads which share pid should point same sz
    acquire(&ptable.lock);
    for(p=ptable.proc ; p<&ptable.proc[NPROC] ; p++)
        if(p->pid == curproc->pid)
            p->sz = sz;
    release(&ptable.lock);

    switchuvm(curproc);
    return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
    int i, pid;
    struct proc *np;
    struct proc *curproc = myproc();

    // Allocate process.
    if((np = allocproc()) == 0){
        return -1;
    }

    // Copy process state from proc.
    if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
        kfree(np->kstack);
        np->kstack = 0;
        np->state = UNUSED;
        return -1;
    }
    np->sz = curproc->sz;
    np->usz = curproc->usz;
    np->parent = curproc;
    *np->tf = *curproc->tf;

    // Clear %eax so that fork returns 0 in the child.
    np->tf->eax = 0;

    for(i = 0; i < NOFILE; i++)
        if(curproc->ofile[i])
            np->ofile[i] = filedup(curproc->ofile[i]);
    np->cwd = idup(curproc->cwd);

    safestrcpy(np->name, curproc->name, sizeof(curproc->name));

    pid = np->pid;

    acquire(&ptable.lock);

    np->state = RUNNABLE;

    release(&ptable.lock);

    return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
    struct proc *curproc = myproc();
    struct proc *p;
    int fd;

    if(curproc == initproc)
        panic("init exiting");

    // Close all open files of mthread and wthreads
    for(p=ptable.proc ; p<&ptable.proc[NPROC] ; p++){
        if(p->pid == curproc->pid){
            for(fd = 0; fd < NOFILE; fd++){
                if(p->ofile[fd]){
                    fileclose(p->ofile[fd]);
                    p->ofile[fd] = 0;
                }
            }
            begin_op();
            iput(p->cwd);
            end_op();
            p->cwd = 0;
        }
    }

    acquire(&ptable.lock);

    // Parent might be sleeping in wait().
    wakeup1(curproc->parent);

    for(p=ptable.proc; p<&ptable.proc[NPROC]; p++){
        if(p->pid==curproc->pid && p->state!=UNUSED)
            p->state = ZOMBIE;

        // Child which is mthread
        if(p->parent == curproc){
            p->parent = initproc;
            if(p->state == ZOMBIE){
                wakeup1(initproc);
            }
        }
    }

    // Adjust the tickets and MLFQ_stride in exit
    MLFQ_tickets += curproc->total_tickets;
    stride_tickets -= curproc->total_tickets;
    MLFQ_stride = (double)100 / MLFQ_tickets;

    // Jump into the scheduler, never to return.
    sched();
    panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Clean up all wthreads whose parent is curproc
// Return -1 if this process has no children.
int
wait(void)
{
    struct proc *p;
    int havekids, pid;
    struct proc *curproc = myproc();


    acquire(&ptable.lock);
    for(;;){
        // Scan through table looking for exited children.
        havekids = 0;
        // Clean up all zombie(exit or thread_exit made it zombie) wthreads
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
            if(p->parent==curproc && p->is_thread && p->state==ZOMBIE){
                kfree(p->kstack);
                p->kstack = 0;

                p->pid = 0;
                p->parent = 0;
                p->name[0] = 0;
                p->killed = 0;
                p->is_thread = 0;
                tid_alloc[p->tid-1] = 0;
                p->tid = 0;
                p->state = UNUSED;
            }
        }
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
            if(p->is_thread || p->parent!=curproc)
                continue;
            havekids = 1;
            if(p->state == ZOMBIE){
                // Found one.
                pid = p->pid;
                kfree(p->kstack);
                p->kstack = 0;
                freevm(p->pgdir);

                p->pid = 0;
                p->parent = 0;
                p->name[0] = 0;
                p->killed = 0;
                p->state = UNUSED;
                release(&ptable.lock);
                return pid;
            }
        }

        // No point waiting if we don't have any children.
        if(!havekids || curproc->killed){
            release(&ptable.lock);
            return -1;
        }

        // Wait for children to exit.  (See wakeup1 call in proc_exit.)
        sleep(curproc, &ptable.lock);  //DOC: wait-sleep
    }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose which scheduler to run : MLFQ_scheduler, stride_scheduler
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.

// stride_scheduler picks the process which has min pass value
// It only checks the process which called set_cpu_share(cannot exceed 80)
void
stride_scheduler(void)
{
    struct proc *p;
    struct cpu *c = mycpu();

    c->proc = 0;

    double min_pass =  10000000000;
    int min_pass_idx = 0;  // Index for process which has min pass

    // Find the process which has minimum pass value
    // That process will be switched
    for(int i=0 ; i<64 ; i++){
        p = ptable.proc+i;
        if(p->state!=RUNNABLE || p->mlfq_mode==1)
            continue;
        if(p->pass >= min_pass)
            continue;

        min_pass = p->pass;
        min_pass_idx = i;
    }
    p = ptable.proc+min_pass_idx;

    c->proc = p;

    switchuvm(p);

    // Increment p's pass and stride_pass for fairness
    p->pass += p->stride;
    stride_pass += p->stride;
    p->state = RUNNING;

    swtch(&(c->scheduler), p->context);
    switchkvm();

    c->proc = 0;
}

// Indices for ptable search
int prior0_idx=0;   // index for prioriy0 processes
int prior1_idx=0;   // index for prioriy1 processes
int prior2_idx=0;   // index for prioriy2 processes

// MLFQ_scheduler picks the process which is in MLFQ
// Each level of queue adopts Round Robin policy with different time quantum.
// level0 : 1ticks, level1 : 2ticks, level2 : 4ticks
// Each queue has different time allotment.
// level0 : 5ticks, level1 : 10ticks
// No pyhsical queue is used, but logical queue is used by priorN_idx
// Prirority boosting evoked every 100ticks
// When set_cpu_share called MLFQ_pass increments according to level
// ->level0 : +MLFQ_stride, level1 : +2*MLFQ_stride, level2 : +4*MLFQ_stride
void
MLFQ_scheduler(void)
{
    struct proc *p;
    struct cpu *c= mycpu();

    // Priority boosting : make all mlfq_mode processes' level, quantum, allotment 0
    if(boosting_ticks >= 100){
        for(p=ptable.proc ; p<&ptable.proc[NPROC] ; p++){
            if(p->mlfq_mode==1){
                p->prior_level = 0;
                p->quantum = 0;
                p->allotment = 0;
            }
        }
        boosting_ticks = 0;
    }
    c->proc = 0;

    // Search level0
    for(int i=0 ; i<64 ; i++){
        if(prior0_idx == 64)
            prior0_idx = 0;

        p = ptable.proc+prior0_idx++;
        if(p->state!=RUNNABLE || p->mlfq_mode!=1 || p->prior_level!=0)
            continue;

        c->proc = p;

        switchuvm(p);
        // Increment MLFQ_pass only when set_cpu_share called
        if(MLFQ_tickets!=100)
            MLFQ_pass += MLFQ_stride;
        // Increment process priority level by checking proc->allotment
        if(p->allotment >= 5){
            p->prior_level++;
            p->quantum = 0;
            p->allotment = 0;
        }
        p->state = RUNNING;

        swtch(&(c->scheduler), p->context);
        switchkvm();
        c->proc = 0;
    }
    for(int i=0 ; i<64 ; i++){
        if(prior1_idx == 64)
            prior1_idx = 0;

        p = ptable.proc+prior1_idx++;
        if(p->state!=RUNNABLE || p->mlfq_mode!=1 || p->prior_level!=1)
            continue;

        c->proc = p;

        switchuvm(p);

        if(MLFQ_tickets!=100)
            MLFQ_pass += 2 * MLFQ_stride;
        if(p->allotment >= 10){
            p->prior_level++;
            p->quantum = 0;
            p->allotment = 0;
        }
        p->state = RUNNING;

        swtch(&(c->scheduler), p->context);
        switchkvm();

        c->proc = 0;
    }
    for(int i=0 ; i<64 ; i++){
        if(prior2_idx == 64)
            prior2_idx = 0;

        p = ptable.proc+prior2_idx++;
        if(p->state!=RUNNABLE || p->mlfq_mode!=1 || p->prior_level!=2)
            continue;

        c->proc = p;

        switchuvm(p);
        if(MLFQ_tickets!=100)
            MLFQ_pass += 4 * MLFQ_stride;

        p->state = RUNNING;

        swtch(&(c->scheduler), p->context);
        switchkvm();

        c->proc = 0;
    }
    // When only stride mode processes run, no MLFQ_mode process is picked. So, MLFQ_pass should be incremented
    if(MLFQ_tickets != 100)
        MLFQ_pass += MLFQ_stride;
}

void
scheduler(void)
{
    for(;;){
        // Enable interrupts on this processor.
        sti();

        acquire(&ptable.lock);
        // MLFQ_tickest==100 <-> no set_cpu_share called
        // -> MLFQ_scheduler
        if(MLFQ_tickets==100){
            MLFQ_scheduler();
        }
        // set_cpu_share called, and MLFQ pass <= stride_pass
        else if(MLFQ_tickets!=100 && MLFQ_pass<=stride_pass){
            MLFQ_scheduler();
        }
        // set_cpu_share called, and MLFQ pass > stride_pass
        else{
            stride_scheduler();
        }

        release(&ptable.lock);
    }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
    int intena;
    struct proc *p = myproc();

    if(!holding(&ptable.lock))
    panic("sched ptable.lock");
    if(mycpu()->ncli != 1)
    panic("sched locks");
    if(p->state == RUNNING)
    panic("sched running");
    if(readeflags()&FL_IF)
    panic("sched interruptible");
    intena = mycpu()->intena;
    swtch(&p->context, mycpu()->scheduler);
    mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
    acquire(&ptable.lock);  //DOC: yieldlock
    myproc()->state = RUNNABLE;
    sched();
    release(&ptable.lock);
}

int
set_cpu_share(int n){
    struct proc * p;
    struct proc * curproc = myproc();

    // If stride's cpu share exceeds 80 -> reject
    if(MLFQ_tickets-n < 20)
        return -1;

    // Adjust some values of process
    // also global tickets and stride
    acquire(&ptable.lock);
    for(p=ptable.proc ; p<&ptable.proc[NPROC] ; p++){
        if(p->pid == curproc->pid){
            p->mlfq_mode = 0;
            p->prior_level = -1;
            p->total_tickets = (double)n;
            p->tickets = (double)n/(double)(curproc->mthread->num_thread+1);
            p->stride = (double)100/p->tickets;
        }
    }

    MLFQ_tickets -= n;
    stride_tickets += n;

    MLFQ_stride = (double)100/MLFQ_tickets;

    MLFQ_pass = 0;
    stride_pass = 0;

    // Initialize all processes' pass as 0
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if(p->mlfq_mode == 0)
            p->pass = 0;

    release(&ptable.lock);

    return 0;
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
    static int first = 1;
    // Still holding ptable.lock from scheduler.
    release(&ptable.lock);

    if (first) {
        // Some initialization functions must be run in the context
        // of a regular process (e.g., they call sleep), and thus cannot
        // be run from main().
        first = 0;
        iinit(ROOTDEV);
        initlog(ROOTDEV);
    }

    // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
    struct proc *p = myproc();

    if(p == 0)
    panic("sleep");

    if(lk == 0)
    panic("sleep without lk");

    // Must acquire ptable.lock in order to
    // change p->state and then call sched.
    // Once we hold ptable.lock, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup runs with ptable.lock locked),
    // so it's okay to release lk.
    if(lk != &ptable.lock){  //DOC: sleeplock0
        acquire(&ptable.lock);  //DOC: sleeplock1
        release(lk);
    }
    // Go to sleep.
    p->chan = chan;
    p->state = SLEEPING;

    sched();

    // Tidy up.
    p->chan = 0;

    // Reacquire original lock.
    if(lk != &ptable.lock){  //DOC: sleeplock2
        release(&ptable.lock);
        acquire(lk);
    }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
    struct proc *p;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if(p->state == SLEEPING && p->chan == chan)
            p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
    acquire(&ptable.lock);
    wakeup1(chan);
    release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
    struct proc *p;

    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->pid == pid){
            p->killed = 1;
            // Wake process from sleep if necessary.
            if(p->state == SLEEPING)
                p->state = RUNNABLE;
            release(&ptable.lock);
            return 0;
        }
    }
    release(&ptable.lock);
    return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
    static char *states[] = {
        [UNUSED]    "unused",
        [EMBRYO]    "embryo",
        [SLEEPING]  "sleep ",
        [RUNNABLE]  "runble",
        [RUNNING]   "run   ",
        [ZOMBIE]    "zombie"
    };
    int i;
    struct proc *p;
    char *state;
    uint pc[10];

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state == UNUSED)
        continue;
        if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
        state = states[p->state];
        else
        state = "???";
        cprintf("%d %s %s", p->pid, state, p->name);
        if(p->state == SLEEPING){
            getcallerpcs((uint*)p->context->ebp+2, pc);
            for(i=0; i<10 && pc[i] != 0; i++)
            cprintf(" %p", pc[i]);
        }
        cprintf("\n");
    }
}

int
thread_create(thread_t * thread, void * (* start_routine)(void *), void * arg)
{
    struct proc * np;
    struct proc * p;
    struct proc * curproc = myproc();
    uint sz, sp, ustack[2];

    if((np=allocproc()) == 0)
        return -1;

    // Duplicate mthread's ofile, cwd, name
    for(int i=0 ; i<NOFILE ; i++)
        if(curproc->ofile[i])
            np->ofile[i] = filedup(curproc->ofile[i]);

    np->cwd = idup(curproc->cwd);

    safestrcpy(np->name, curproc->name, sizeof(curproc->name));

    np->sz = curproc->sz;
    np->pgdir = curproc->pgdir;
    np->pid = curproc->pid;
    np->parent = curproc->parent;
    // If wthread call thread_create()
    // -> it is same as mthread calls thread_create()
    // -> np's mthread will be wthread's mthread
    if(curproc->is_thread)
        np->mthread = curproc->mthread;
    else
        np->mthread = curproc;
    np->mthread->num_thread++;
    *np->tf = *curproc->tf;
    np->is_thread = 1;

    // Allocate available tid by searching tid_alloc[]
    for(int i=0 ; i<NPROC ; i++){
        if(tid_alloc[i] == 0){
            np->tid = i+1;
            tid_alloc[i] = 1;
            break;
        }
    }

    *thread = np->tid;

    // Set its sz and usz. Only ustack[2] is needed for start_routine's argument
    sz = curproc->usz + (uint)(2*PGSIZE*(np->tid));
    sp = sz;
    np->usz = sz;

    ustack[0] = 0xFFFFFFFF;
    ustack[1] = (uint)arg;

    sp -= 8;
    if(copyout(np->pgdir, sp, ustack, 8) < 0)
        return -1;

    // Set eip as an address of start_routine()
    np->tf->eip = (uint)start_routine;
    np->tf->esp = sp;

    // Reallocate total_tickets, tickets and stride
    acquire(&ptable.lock);
    if(curproc->total_tickets){
        for(p=ptable.proc ; p<&ptable.proc[NPROC] ; p++){
            if(p->pid == curproc->pid){
                p->mlfq_mode = 0;
                p->prior_level = -1;
                p->total_tickets = curproc->total_tickets;
                p->tickets = curproc->total_tickets / (double)(curproc->num_thread+1);
                p->stride = (double)100 / p->tickets;
            }
        }
        // Initialize all processes' pass as 0
        // because new stride mode LWP is created
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
            p->pass = 0;

        MLFQ_pass = 0;
        stride_pass = 0;
    }
    np->state = RUNNABLE;
    release(&ptable.lock);

    return 0;
}

void
thread_exit(void * ret_val)
{
    struct proc * p;
    struct proc * curproc = myproc();
    int fd;

    if(!curproc->is_thread)
        panic("mthread cannot call thread_exit()");

    // Close ofile of wthread
    for(fd=0 ; fd<NOFILE ; fd++){
        if(curproc->ofile[fd]){
            fileclose(curproc->ofile[fd]);
            curproc->ofile[fd] = 0;
        }
    }
    begin_op();
    iput(curproc->cwd);
    end_op();
    curproc->cwd = 0;

    // Store ret_val to wthread->ret_val
    // It will be read in thread_join later
    curproc->ret_val = (int)ret_val;

    acquire(&ptable.lock);
    // Wake mthread to clean wthread's resources
    wakeup1(curproc->mthread);

    curproc->mthread->num_thread--;
    // Reallocate total_tickets, tickets and stride
    for(p=ptable.proc ; p<&ptable.proc[NPROC] ; p++){
        if(curproc->total_tickets){
            for(p=ptable.proc ; p<&ptable.proc[NPROC] ; p++){
                if(p->pid==curproc->pid && p->tid!=curproc->tid){
                    p->tickets = curproc->total_tickets / (double)(curproc->mthread->num_thread+1);
                    p->stride = (double)100 / p->tickets;
                }
            }
        }
    }

    curproc->state = ZOMBIE;

    sched();
    panic("zombie exit");
}

int
thread_join(thread_t thread, void ** retval)
{
    struct proc * p;
    struct proc * curproc = myproc();

    if(curproc->is_thread)
        panic("wthread cannot call thread_join()");

    acquire(&ptable.lock);
    for(;;){
        for(p=ptable.proc ; p<&ptable.proc[NPROC] ; p++){
            // Skip mthreads / wthreads which doesn't have thread as tid
            if(!p->is_thread || p->tid!=thread)
                continue;

            if(p->state == ZOMBIE){
                * retval = (void*)p->ret_val;

                kfree(p->kstack);
                p->kstack = 0;

                p->pid = 0;
                p->parent = 0;
                p->name[0] = 0;
                p->killed = 0;
                p->is_thread = 0;
                // Return its tid
                tid_alloc[p->tid-1] = 0;
                p->tid = 0;
                p->state = UNUSED;

                release(&ptable.lock);
                return 0;
            }
        }
        if(curproc->killed){
            release(&ptable.lock);
            return -1;
        }

        // Wait for wthread to exit
        sleep(curproc, &ptable.lock);
    }
}
