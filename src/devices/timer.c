#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "filesys/cache.h"
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* ADDED FUNCTIONS */
static bool sleep_less_func(const struct list_elem *a,
			    const struct list_elem *b,
			    void *aux);

/* ADDED DEFINITIONA */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) 
{
  /* 8254 input frequency divided by TIMER_FREQ, rounded to
     nearest. */
  uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

  outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
  outb (0x40, count & 0xff);
  outb (0x40, count >> 8);

  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  barrier ();
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/************************************************************************
* FUNCTION : timer_sleep                                               *
* Input : NONE                                                          *
* Output : NONE                                                         *
* Purporse : Suspends execution for approximately TICKS timer ticks.    *
*************************************************************************/
void
timer_sleep (int64_t ticks) 
{
  int64_t start = timer_ticks ();
  if (ticks < 0)
    return;
  ASSERT (intr_get_level () == INTR_ON);
  enum intr_level old_level = intr_disable();
  struct thread *current_thread = thread_current();
  current_thread -> wakeup_time = start + ticks;
  list_insert_ordered(&sleep_list, &(current_thread -> elem),
		      (list_less_func *) &sleep_less_func, NULL);
  thread_block();
  intr_set_level(old_level);
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}


/************************************************************************
* FUNCTION : timer_interrupt                                            *
* Input : NONE                                                          *
* Output : NONE                                                         *
* Purporse : For Every Tick interrupt handler is activated              *
* used for waking thread up or updating priority in mlfqs               *
************************************************************************/
/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  struct thread *sleeptemp = NULL; //added
  struct list_elem *iter = NULL; // added
  ticks++;
  int a=0;

  thread_tick();

  /* for mlfqs*/
  if(thread_mlfqs)
  {
    //recent_cpu of current thread add one in fixed point0
    increment_recent_cpu(thread_current());

    // For every second update load_avg and recent_cpu of current_thread
    if (timer_ticks() % TIMER_FREQ == 0)
    {
      //Update load_avg
      update_load_avg();
      //Calculate recent_cpu fo all threads
      update_recent_cpus();
    }
    if (timer_ticks() % TIME_SLICE == 0)
    {
      //Calculate Priority 
      update_priorities();
      intr_yield_on_return ();
    }
  }

  // if (timer_ticks() % (5*TIMER_FREQ) == 0)
  // {
  //   buffer_cache_free();
  // }

  //unblock threads that has passed wakeup_time
  for(iter = list_begin(&sleep_list);
    iter != list_tail(&sleep_list); iter = list_begin(&sleep_list))
  {
    sleeptemp = list_entry(list_front(&sleep_list), struct thread, elem);
    if (sleeptemp->wakeup_time <= ticks)
    {
      list_pop_front(&sleep_list);
      thread_unblock(sleeptemp);
    }
    else
    {
      break;
    }
  }
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing.  We scale the numerator and denominator
         down by 1000 to avoid the possibility of overflow. */
      ASSERT (denom % 1000 == 0);
      busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
    }
}

/************************************************************************
* FUNCTION : sleep_less_func                                            *
* Purporse : Used for list element comparison funcfion for Sleep list   * 
************************************************************************/
static bool
sleep_less_func(const struct list_elem *a, const struct list_elem *b, void *aux)
{
  struct thread *alpha = list_entry(a, struct thread, elem);
  struct thread *beta = list_entry(b, struct thread, elem);

  if (alpha->wakeup_time < beta->wakeup_time)
    {
      return true;
    }
  else
    {
      return false;
    }
}
