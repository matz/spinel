# spinel: not-cruby -- ffi_func and offload: are Spinel's own
# An ffi_func declared `offload: true` runs on another OS thread while the
# calling thread waits as on I/O: another thread, on any worker, runs
# meanwhile. Here the offloaded call waits for a flag that only the other
# thread's call sets, so the program ends only if that thread runs while
# the call waits. The answer, errno and every argument and result type come
# back as from a plain call.
module Gate
  ffi_source <<~C
    #include <errno.h>
    #include <time.h>
    #include <string.h>
    static volatile int gate_open;
    int gate_wait(int n) {
      struct timespec ts = { 0, 1000000L };
      while (!__atomic_load_n(&gate_open, __ATOMIC_ACQUIRE)) nanosleep(&ts, NULL);
      return n * 2;
    }
    void gate_set(void) { __atomic_store_n(&gate_open, 1, __ATOMIC_RELEASE); }
    double gate_half(double x) { return x / 2; }
    long gate_len(const char *s) { return (long)strlen(s); }
    int gate_fail(void) { errno = ENOENT; return -1; }
  C
  ffi_func :gate_wait, [:int], :int, offload: true
  ffi_func :gate_set, [], :void
  ffi_func :gate_half, [:double], :double, offload: true
  ffi_func :gate_len, [:str], :long, offload: true
  ffi_func :gate_fail, [], :int, offload: true
end

waiter = Thread.new { Gate.gate_wait(21) }
opener = Thread.new { sleep 0.05; Gate.gate_set; :opened }
p opener.value
p waiter.value
p Gate.gate_half(5.0)
p Gate.gate_len("offload")
p Gate.gate_fail
p 4.times.map { |i| Thread.new { Gate.gate_wait(i) } }.map(&:value)
