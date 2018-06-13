// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

int tid_alloc[NPROC];  // Array for tid allocation

uint boosting_ticks;  // Ticks for priority boosting
double MLFQ_pass;
double MLFQ_stride;
double stride_pass;  // Sum of all stride mode processes' tickets / Different from proc's pass

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  uint usz;
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct proc *mthread;        // mthread : process which called thread_create()
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  int mlfq_mode;               // 1 : mlfq, 0 : stride
  int prior_level;             // 0~2 : mlfq, -1 : stride
  uint quantum;                // For checking time quantum
  uint allotment;              // For checking time allotment
  int total_tickets;                 // Represent cpu_share, 0 for mlfq_mode processes
  double tickets;
  double stride;                  // stride = 10,000 / tickets, 0 for mlfq_mode processes
  double pass;                    // 0 for mlfq_mode processes
  int is_thread;               // 0 = mthread, 1 = wthread
  thread_t tid;                // 0 for mthreads
  int num_thread;           // number of wthread that mthread has / o for wthread
  int ret_val;                 // ret_val used in thread_exit(), thread_join()
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
