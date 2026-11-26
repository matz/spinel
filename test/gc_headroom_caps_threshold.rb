# After a collection the next threshold is twice the bytes that survived, so
# a program holding a large live set lets as much garbage again pile up before
# it collects: 600 MB of live data collects at 1.2 GB however little is
# allocated in between. SPINEL_GC_HEADROOM_KB caps what the doubling adds:
# the threshold is then the survivors plus at most that much.
#
# The collection controls are read again when the first Thread sizes the
# budget for the worker pool, which is how this program sets its own.
#
# Asserted as properties: the byte counts depend on the allocator's tuning.

ENV["SPINEL_WORKERS"] = "1"
ENV["SPINEL_GC_HEADROOM_KB"] = "1024"
Thread.new { 1 + 1 }.join

keep = []
600.times { |i| keep << Array.new(4096, i * 1.0) }   # about 20 MB live
GC.start
st = GC.stat
room = st["threshold"] - st["bytes"]

# Twice the survivors would leave about 20 MB of room; the cap leaves 1 MB.
p st["bytes"] > 16 * 1024 * 1024
p room <= 1024 * 1024

# Collection still happens against it, and the heap stays near the live set.
c0 = GC.stat["cycle"]
3000.times do
  a = Array.new(8192, 1.0)
  a[0] = 2.0
end
p GC.stat["cycle"] > c0
p GC.stat["bytes"] < st["bytes"] + 4 * 1024 * 1024

p keep.length
