# GC: a Method captures `self`, and the captured receiver may
# be reachable only through the Method itself. The auto-generated
# `sp_Method_gc_scan` walks `iv_self_obj` via `sp_gc_mark`, so
# the receiver survives any GC cycle that runs before `bm.call`.
#
# Counter has only an int ivar, which would normally make it a
# value-type class — but `detect_method_taken_classes` excludes it
# so `self` stays a heap-allocated pointer that the Method can
# safely hold.

class Counter
  def initialize(start)
    @n = start
  end
  def add(x)
    @n + x
  end
  def own_bm
    method(:add)
  end
end

def make_bm
  c = Counter.new(100)
  c.own_bm  # the only handle to `c` is via the returned Method
end

bm = make_bm
1000.times { String.new("garbage" * 100) }
puts bm.call(7)
