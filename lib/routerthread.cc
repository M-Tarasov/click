#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <click/glue.hh>
#include <click/router.hh>
#include <click/routerthread.hh>
#ifdef __KERNEL__
extern "C" {
#include <linux/sched.h>
}
#endif

#define DEBUG_RT_SCHED		0

#define DRIVER_TASKS_PER_ITER	128
#define PROFILE_ELEMENT		20

#define DRIVER_ITER_ANY		32
#define DRIVER_ITER_TIMERS	32
#define DRIVER_ITER_SELECT	64
#define DRIVER_ITER_LINUXSCHED	256


RouterThread::RouterThread(Router *r)
  : Task(Task::error_hook, 0), _router(r)
{
  _prev = _next = _list = this;
  router()->add_thread(this);
  // add_thread() will call this->set_thread_id()
}

RouterThread::~RouterThread()
{
  router()->remove_thread(this);
}

void
RouterThread::add_task_request(unsigned op, Task *t)
{
  _taskreq_lock.acquire();
  _taskreq_ops.push_back(op);
  _taskreq_tasks.push_back(t);
  _taskreq_lock.release();
}

inline void
RouterThread::process_task_requests()
{
  Vector<unsigned> ops;
  Vector<Task *> tasks;

  _taskreq_lock.acquire();
  ops.swap(_taskreq_ops);
  tasks.swap(_taskreq_tasks);
  _taskreq_lock.release();

  for (int i = 0; i < ops.size(); i++) {
    Task *t = tasks[i];
    switch (ops[i]) {
      
     case SCHEDULE_TASK:
      if (t->scheduled_list() == this)
	t->reschedule();
      break;
      
     case UNSCHEDULE_TASK:
      if (t->scheduled() && t->scheduled_list() == this) 
	t->fast_unschedule();
      break;

#if __MTCLICK__
     case MOVE_TASK:
      if (t->scheduled_list() == this)
	t->fast_change_thread();
      break;
#endif

    }
  }
}

#if __MTCLICK__

void
RouterThread::driver()
{
  const volatile int * const runcount = _router->driver_runcount_ptr();
  u_int64_t cycles = 0;
  int iter = 0;
  Task *t;
  
  lock_tasks();

  do {
    
    while (*runcount > 0) {
      // run a bunch of tasks
      int c = DRIVER_TASKS_PER_ITER;
      while ((t = scheduled_next()),
	     t != this && c >= 0) {
	int runs = t->fast_unschedule();
	if (runs > PROFILE_ELEMENT)
	  cycles = click_get_cycles();
	t->call_hook();
	if (runs > PROFILE_ELEMENT) {
	  cycles = click_get_cycles() - cycles;
	  cycles = ((unsigned)cycles)/32 + ((unsigned)t->cycles())*31/32;
	  t->update_cycles(cycles);
	}
	c--;
      }

      // check _driver_runcount
      if (*runcount <= 0)
	break;

      // run task requests
      if (_taskreq_ops.size())
	process_task_requests();

      // run occasional tasks: task requests, timers, select, etc.
      iter++;
      if (iter % DRIVER_ITER_ANY == 0)
	wait(iter);
    }

  } while (_router->check_driver());
  
  unlock_tasks();
}

#else

void
RouterThread::driver()
{
  const volatile int * const runcount = _router->driver_runcount_ptr();
  int iter = 0;
  Task *t;
  
  lock_tasks();

  do {
    
    while (*runcount > 0) {
      // run a bunch of tasks
      int c = DRIVER_TASKS_PER_ITER;
      while ((t = scheduled_next()),
	     t != this && c >= 0) {
	t->fast_unschedule();
	t->call_hook();
	c--;
      }

      // check _driver_runcount
      if (*runcount <= 0)
	break;

      // run task requests
      if (_taskreq_ops.size())
	process_task_requests();
    
      // run occasional tasks: timers, select, etc.
      iter++;
      if (iter % DRIVER_ITER_ANY == 0)
	wait(iter);
    }

  } while (_router->check_driver());

  unlock_tasks();
}

#endif

void
RouterThread::driver_once()
{
  if (!_router->check_driver())
    return;
  
  lock_tasks();
  Task *t = scheduled_next();
  if (t != this) {
    t->unschedule();
    t->call_hook();
  }
  unlock_tasks();
}

void
RouterThread::wait(int iter)
{
  if (thread_id() == 0) {
    unlock_tasks();
#if CLICK_USERLEVEL
    if (iter % DRIVER_ITER_SELECT == 0)
      router()->run_selects(!empty());
#else /* __KERNEL__ */
    if (iter % DRIVER_ITER_LINUXSCHED == 0) 
      schedule();
#endif
    lock_tasks();
  }

  if (iter % DRIVER_ITER_TIMERS == 0)
    router()->run_timers();
}
