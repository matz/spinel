# spinel: not-cruby -- ffi_func and ffi_source are Spinel's own
# A thread a helper spawned waits on that helper's queue, and the helper here sits
# in a blocking call until that thread has run. Main's Thread.pass brings up
# another helper for it.
module C
  ffi_source <<~S
    #include <unistd.h>
    static volatile int g_flag;
    void cw_flag(void) { g_flag = 1; }
    /* up to 30 s, so a thread that never runs fails the test and does not hang it */
    int cw_wait(void) { for (int w = 0; w < 30000 && !g_flag; w++) usleep(1000); return g_flag; }
  S
  ffi_func :cw_flag, [], :void
  ffi_func :cw_wait, [], :int, blocking: true
end

$seen = -1
parent = Thread.new do
  Thread.new { C.cw_flag }
  $seen = C.cw_wait
end
Thread.pass while $seen < 0
parent.join
p $seen
