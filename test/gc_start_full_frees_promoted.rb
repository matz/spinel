# GC.start is a full collection, as CRuby's is: an Array that minor cycles
# promoted while it was being built, then dropped, is freed by it. It ran
# the collector on its cadence, a minor cycle that keeps old objects, so the
# last structure a method built stayed until the next full cycle, and a
# test that measured a release (proc_arg_channel_released) depended on how
# large that structure was on the platform (it failed on -m32). The count
# is GC.stat's heap_live_slots, the objects the heap holds, which CRuby
# keeps too.
def build(len)
  n = len.to_i
  a = []
  i = 0
  while i < n
    a << [i, i]
    i += 1
  end
  a
end
def live_slots(n)
  a = build(n)
  held = GC.stat(:heap_live_slots) - $base
  a.size > 0 ? held : 0   # (a stays live until the count is read)
end
GC.start
$base = GC.stat(:heap_live_slots)
held = live_slots(20_000)
GC.start
freed = GC.stat(:heap_live_slots) - $base
p held > 10_000
p freed < held / 4
p GC.stat(:heap_live_slots).is_a?(Integer), GC.stat.key?(:heap_live_slots) || GC.stat.key?("heap_live_slots")
