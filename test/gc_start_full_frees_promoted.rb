# spinel: not-cruby -- GC.stat["bytes"] is the runtime's own heap count; CRuby's GC.stat has no such key
# GC.start is a full collection, as CRuby's is: an Array that minor cycles
# promoted while it was being built, then dropped, is freed by it. It ran
# the collector on its cadence, a minor cycle that keeps old objects, so the
# last structure a method built stayed until the next full cycle, and a
# test that measured a release (proc_arg_channel_released) depended on how
# large that structure was on the platform (it failed on -m32).
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
def live_bytes(n)
  a = build(n)
  held = GC.stat["bytes"] - $base
  a.size > 0 ? held : 0   # (a stays live until the bytes are read)
end
$base = GC.stat["bytes"]
held = live_bytes(20_000)
GC.start
freed = GC.stat["bytes"] - $base
p held > 100_000
p freed < held / 4
