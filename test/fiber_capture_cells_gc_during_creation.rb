# Fiber capture cells must survive a GC that strikes during fiber
# construction. A `Fiber.new { ... }` with captured locals lowers to a
# run of back-to-back sp_gc_alloc calls: one heap cell per captured
# variable, then the capture struct, then sp_Fiber_new's own fiber
# allocation. Pre-fix none of those intermediates was rooted, so any
# allocation in that window crossing the GC threshold collected the
# cells (and/or the capture struct) already allocated; the assembled
# fiber then read and wrote freed memory on its first resume. With
# enough fiber-creation churn the threshold crossing eventually lands
# inside the window: a compiled HTTP server with one fiber per
# connection died deterministically after ~297 connections (SIGSEGV at
# -O2; "undefined method ... for nil" at -O0, the freed cell's memory
# having been recycled by a zeroing allocation).
#
# Each round captures five Holder objects and almost nothing else
# allocates, so virtually every GC trigger lands inside the
# fiber-construction window; pre-fix this segfaulted in well under
# 50000 rounds.

class Holder
  def initialize(n)
    @n = n
  end

  def n
    @n
  end
end

def run_one(i)
  a = Holder.new(i)
  b = Holder.new(i + 1)
  c = Holder.new(i + 2)
  d = Holder.new(i + 3)
  e = Holder.new(i + 4)
  f = Fiber.new do
    a.n + b.n + c.n + d.n + e.n
  end
  f.resume
end

def churn(rounds)
  total = 0
  i = 0
  while i < rounds
    total = total + run_one(i)
    i = i + 1
  end
  total
end

puts "total=" + churn(50000).to_s
