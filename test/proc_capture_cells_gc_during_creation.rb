# Proc capture cells must survive a GC that strikes during proc
# construction — the Proc.new sibling of
# fiber_capture_cells_gc_during_creation.rb. A `Proc.new { ... }` with
# captured locals allocates one heap cell per captured variable, then
# the capture struct, then the proc object itself; pre-fix none of the
# intermediates was rooted, so a GC threshold crossing inside that
# allocation window freed the cells already allocated and the proc
# body then read freed memory. Pre-fix this segfaulted in well under
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
  p = Proc.new do
    a.n + b.n + c.n + d.n + e.n
  end
  p.call
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
