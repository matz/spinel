# spinel: not-cruby -- ffi_func and ffi_source are Spinel's own
# A thread that has not run yet waits for a worker while the only helper sits
# in a blocking call that waits for that thread. It runs on main, whether it
# was queued before the call began or after.
ENV["SPINEL_WORKERS"] = "2"   # one helper, under any outside setting
module C
  ffi_source <<~S
    #include <unistd.h>
    #include <time.h>
    static int g_flag;
    void cw_reset(void) { __atomic_store_n(&g_flag, 0, __ATOMIC_RELEASE); }
    void cw_flag(void) { __atomic_store_n(&g_flag, 1, __ATOMIC_RELEASE); }
    /* a busy wait the scheduler cannot preempt: no safepoint, no blocking call */
    void cw_spin(int ms) { struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
      do clock_gettime(CLOCK_MONOTONIC, &t); while ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < ms); }
    /* up to 5 s, so a thread that never runs fails the test and does not hang it */
    int cw_wait(void) {
      for (int w = 0; w < 5000 && !__atomic_load_n(&g_flag, __ATOMIC_ACQUIRE); w++) usleep(1000);
      return __atomic_load_n(&g_flag, __ATOMIC_ACQUIRE);
    }
  S
  ffi_func :cw_reset, [], :void
  ffi_func :cw_flag, [], :void
  ffi_func :cw_spin, [:int], :void
  ffi_func :cw_wait, [], :int, blocking: true
end

def started_after_the_call
  C.cw_reset
  seen = -1
  parent = Thread.new { seen = C.cw_wait }
  sleep 0.05
  child = Thread.new { C.cw_flag }
  parent.join
  child.join
  seen
end

def started_together
  C.cw_reset
  seen = -1
  parent = Thread.new { seen = C.cw_wait }
  child = Thread.new { C.cw_flag }
  parent.join
  child.join
  seen
end

def started_by_the_waiter
  C.cw_reset
  seen = -1
  child = nil
  parent = Thread.new do
    child = Thread.new { C.cw_flag }
    seen = C.cw_wait
  end
  parent.join
  child.join
  seen
end

# the waiter starts the child, holds its worker long enough for main to go
# to sleep, then blocks: the child is still on the waiter's own queue
def started_by_the_waiter_before_a_spin
  C.cw_reset
  seen = -1
  child = nil
  parent = Thread.new do
    child = Thread.new { C.cw_flag }
    C.cw_spin(100)
    seen = C.cw_wait
  end
  parent.join
  child.join
  seen
end

p [started_after_the_call, started_together, started_by_the_waiter, started_together,
   started_by_the_waiter_before_a_spin]
