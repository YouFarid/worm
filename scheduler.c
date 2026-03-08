#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED

#include "scheduler.h"

#include <assert.h>
#include <curses.h>
#include <ucontext.h>

#include "util.h"

// This is an upper limit on the number of tasks we can create.
#define MAX_TASKS 128

// This is the size of each task's stack memory
#define STACK_SIZE 65536

enum STAT {Unused = 0, Running, Waiting, Sleeping, Exited, Blocked};  //Self explanotory


// This struct will hold the all the necessary information for each task
typedef struct task_info {
  // This field stores all the state required to switch back to this task
  ucontext_t context;

  // This field stores another context. This one is only used when the task
  // is exiting.
  ucontext_t exit_context;

  //   a. Keep track of this task's state.
  // int state; // 0 unused, 1 running, 2 waiting, 3 sleeping, 4 exited
  enum STAT state;
  //   b. If the task is sleeping, when should it wake up?
  size_t waitingTime; // Ms time
  //   c. If the task is waiting for another task, which task is it waiting for?
  int waitingFor; //Task index 
  //   d. Was the task blocked waiting for user input? Once you successfully
  //      read input, you will need to save it here so it can be returned.
  int inputChar;  //Character read
} task_info_t;

int current_task = 0;          //< The index of the cur running task
int num_tasks = 1;             //< The number of tasks we created so far
task_info_t tasks[MAX_TASKS];  //< Info for every task

/**
 * Initialize the scheduler. Programs should call this before calling any other
 * functiosn in this file.
 */
void scheduler_init() {
  //Loop to initialize all our states
  for (int i = 0; i < MAX_TASKS; i++) {
    tasks[i].state = Unused;
    tasks[i].waitingTime = 0;
    tasks[i].waitingFor = -1;
    tasks[i].inputChar = 0;
  }
  current_task = 0;   //Start task 0
  num_tasks = 1;      //Inc by 1
  tasks[0].state = Running; //Set running
}

// ******************** HELPER FUNCTION ********************
/**
 * Picks up the next runable task and swaqp context with cur +
 * can also wakes up any tasks whose sleep time has expired
 */
static void schedule_next() {
  size_t now = time_ms(); //Cur time

  // Sets states to wake up sleeping tasks whose time has expired
  for (int i = 0; i < num_tasks; i++) {
    if (tasks[i].state == Sleeping && now >= tasks[i].waitingTime) {
      tasks[i].state = Running;
      tasks[i].waitingTime = 0;
    }
  }

  // Search and find next runable task and swap context with cur
  // 90% of errors was in figuring out this part correctly so we added this func first place
  for (int i = 1; i <= num_tasks; i++) {
    int next = (current_task + i) % num_tasks;
    if (tasks[next].state == Running) {
      int prev = current_task;
      current_task = next;
      swapcontext(&tasks[prev].context, &tasks[next].context);
      return;
    }
  }
}
// ******************** END OF HELPER FUNCTION ********************

/**
 * This function will execute when a task's function returns. This allows you
 * to update scheduler states and start another task. This function is run
 * because of how the contexts are set up in the task_create function.
 */
void task_exit() {
  tasks[current_task].state = Exited; //Mark cur as exited
  //Check if anything was waiting for cur to finsh and wake up next
  for (int i = 0; i < num_tasks; i++) {
    if (tasks[i].state == Waiting && tasks[i].waitingFor == current_task) {
      tasks[i].state = Running;
      tasks[i].waitingFor = -1;
    }
  }
  // If all tasks are exited, quit the program cleanly instead of freezing the tereminal
  // Those 3 lines and problem took more time to fix it than writing the code itself I swear 
  
    if (tasks[0].state != Exited) {
    // Transfer list (task 0) to allow it to gameover.
    current_task = 0;
    setcontext(&tasks[0].context); // should not return
  }
}

/**
 * Create a new task and add it to the scheduler.
 *
 * \param handle  The handle for this task will be written to this location.
 * \param fn      The new task will run this function.
 */
void task_create(task_t* handle, task_fn_t fn) {
  // Claim an index for the new task
  int index = num_tasks;
  num_tasks++;

  // Set the task handle to this index, since task_t is just an int
  *handle = index;

  // We're going to make two contexts: one to run the task, and one that runs at the end of the task
  // so we can clean up. Start with the second

  // First, duplicate the current context as a starting point
  getcontext(&tasks[index].exit_context);

  // Set up a stack for the exit context
  tasks[index].exit_context.uc_stack.ss_sp = malloc(STACK_SIZE);
  tasks[index].exit_context.uc_stack.ss_size = STACK_SIZE;

  // Set up a context to run when the task function returns. This should call task_exit.
  makecontext(&tasks[index].exit_context, task_exit, 0);

  // Now we start with the task's actual running context
  getcontext(&tasks[index].context);

  // Allocate a stack for the new task and add it to the context
  tasks[index].context.uc_stack.ss_sp = malloc(STACK_SIZE);
  tasks[index].context.uc_stack.ss_size = STACK_SIZE;

  // Now set the uc_link field, which sets things up so our task will go to the exit context when
  // the task function finishes
  tasks[index].context.uc_link = &tasks[index].exit_context;

  // And finally, set up the context to execute the task function
  makecontext(&tasks[index].context, fn, 0);

  tasks[index].state = Running; //Start running the task we created <- Added part because how we implemented the rest of funcs

}

/**
 * Wait for a task to finish. If the task has not yet finished, the scheduler should
 * suspend this task and wake it up later when the task specified by handle has exited.
 *
 * \param handle  This is the handle produced by task_create
 */
void task_wait(task_t handle) {
  // Only wait if task hasn’t exited yet
  if (tasks[handle].state != Exited) {
    tasks[current_task].state = Waiting;
    tasks[current_task].waitingFor = handle;
    //switch context to next task
    schedule_next();
    // Resume the wait once we finish the other task we waited for
    tasks[current_task].state = Running;
    tasks[current_task].waitingFor = -1;
  }
}

/**
 * The currently-executing task should sleep for a specified time. If that time is larger
 * than zero, the scheduler should suspend this task and run a different task until at least
 * ms milliseconds have elapsed.
 *
 * \param ms  The number of milliseconds the task should sleep.
 */
void task_sleep(size_t ms) {
  if (ms == 0) return;  // sleep time is zero do nothing, fixed the crash condition

  // Set the wakeup time and mark task sleeping
  tasks[current_task].state = Sleeping;
  tasks[current_task].waitingTime = time_ms() + ms;

  //Continue until wake-up time
  while (1) {
    size_t now = time_ms();
    // If enough time has passed, wake up
    if (now >= tasks[current_task].waitingTime) {
      tasks[current_task].state = Running;
      tasks[current_task].waitingTime = 0;
      return; // wake up
    }
    schedule_next(); // allow other tasks to run
  }
}

/**
 * Read a character from user input. If no input is available, the task should
 * block until input becomes available. The scheduler should run a different
 * task while this task is blocked.
 *
 * \returns The read character code
 */
int task_readchar() {
  int c;
  //Loop keep checking for chars, doing something else each time none is available
  while (1) {
    c = getch();
    if (c != ERR) {
      return c; // got input
    }
    // no input yet, yield
    schedule_next();
  }
}


