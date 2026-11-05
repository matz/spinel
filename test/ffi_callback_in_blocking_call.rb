# spinel: not-cruby -- ffi_func and ffi_source are Spinel's own
# An ffi_callback from inside a blocking: true call comes back into the world
# for the method and goes out again after it, whether it was handed to the call
# or to C earlier, as an event loop calls back what was registered with it. The
# method used to run outside the world, beside the collections other workers ran.
module Loop
  ffi_source <<~C
    #include <unistd.h>
    static void (*g_tick)(int);
    static volatile int g_flag;
    void lp_register(void (*f)(int)) { g_tick = f; }
    void lp_run(int n) { for (int i = 0; i < n; i++) { usleep(100); g_tick(i); } }
    int lp_sum(int n, int (*f)(int)) { int s = 0; for (int i = 0; i < n; i++) s += f(i); return s; }
    double lp_half(int (*f)(int), double (*g)(double)) { return g(f(9)); }
    void lp_flag(void) { g_flag = 1; }
    /* one tick, then waits (up to 30 s) with no callback for what only a
       collection can let another thread reach */
    int lp_tick_then_wait(int i) {
      g_flag = 0;
      g_tick(i);
      for (int w = 0; w < 30000 && !g_flag; w++) usleep(1000);
      return g_flag;
    }
  C
  ffi_callback :tick_cb, [:int], :void
  ffi_callback :int_cb, [:int], :int
  ffi_callback :dbl_cb, [:double], :double
  ffi_func :lp_register, [:tick_cb], :void
  ffi_func :lp_flag, [], :void
  ffi_func :lp_run, [:int], :void, blocking: true
  ffi_func :lp_sum, [:int, :int_cb], :int, blocking: true
  ffi_func :lp_half, [:int_cb, :dbl_cb], :double, blocking: true
  ffi_func :lp_tick_then_wait, [:int], :int, blocking: true
end

# The first thread of the program starts inside a blocking call that began with
# no pool. The way out is a real one, or its collection waits for the call.
def first_thread(i)
  $first = Thread.new do
    GC.start
    Loop.lp_flag
  end
end
Loop.lp_register(method(:first_thread))
p Loop.lp_tick_then_wait(0)
$first.join

# Collections run beside the method and what it holds lives through them.
$kept = []
$ticks = 0
$bad = 0
def tick(i)
  x = []
  50.times { |k| x << "cb#{i}-#{k}" }
  $kept << x
  $kept.shift if $kept.size > 8
  $kept.each { |a| a.each_with_index { |s, k| $bad += 1 unless s.end_with?("-#{k}") } }
  $ticks += 1
end
$stop = false
churn = Thread.new do
  n = 0
  until $stop
    a = []
    200.times { |k| a << "item #{k}" }
    n += a.size
  end
  n
end
Loop.lp_register(method(:tick))
Loop.lp_run(200)
puts "ticks #{$ticks}, bad #{$bad}"

# Out again after the method: a collection does not wait for the call.
$flagger = Thread.new do
  3.times { GC.start }
  Loop.lp_flag
end
p Loop.lp_tick_then_wait(0)
$flagger.join

# A callback handed to the call, an answer back to C, and a blocking call
# inside a callback.
def inner(i)
  [i.to_s * 3].first.size + i
end
def outer(i)
  Loop.lp_sum(2, method(:inner)) + i
end
def half(x) = x / 2.0
p Loop.lp_sum(4, method(:inner))
p Loop.lp_sum(3, method(:outer))
p Loop.lp_half(method(:inner), method(:half))

# A raise unwinds out of the blocking call, and the worker is in the world where
# it lands: a blocking call goes on working after it.
def boom(i)
  raise ArgumentError, "tick #{i}" if i == 3
end
Loop.lp_register(method(:boom))
begin
  Loop.lp_run(10)
rescue ArgumentError => e
  puts e.message
end
GC.start
Loop.lp_register(method(:tick))
$ticks = 0
Loop.lp_run(20)
puts "ticks #{$ticks}, bad #{$bad}"

$stop = true
p churn.value > 0
