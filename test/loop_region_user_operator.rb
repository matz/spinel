# spinel: gc-stress
# An operator write calls its operator, and for a variable that holds an
# object that is the program's own method (`w += i` with a class's `+`). A
# loop whose body holds one runs code, so it keeps its arrays' headers out
# of a cache (a `+` that clears or shifts the array the loop reads leaves
# no stale header behind) and its bound is no bound a run may count to (a
# `+` that lowers it ends the loop at the next test). A threaded program,
# so the loops poll.
Thread.new {}.join

# the operator empties or shifts the array the loop reads
class Clearer
  def initialize(a, at); @a = a; @at = at; end
  def +(x); @a.clear if x == @at; self; end
end

class Shifter
  def initialize(a, at); @a = a; @at = at; end
  def +(x); @a.shift(5) if x == @at; self; end
end

def read_after(a, n, w)
  i = 0
  s = 0
  while i < n
    s += a[i] || 100
    w += i
    i += 1
  end
  [i, s]
end

p read_after(Array.new(10) { |k| k }, 8, Clearer.new([], -1))
a = Array.new(10) { |k| k }
p read_after(a, 8, Clearer.new(a, 3))
b = Array.new(10) { |k| k * 10 }
p read_after(b, 8, Shifter.new(b, 2))

# the operator lowers the bound the loop counts to
class Lowering
  def initialize(o, at); @o = o; @at = at; @k = 0; end
  def +(x); @o.limit = 0 if (@k += 1) == @at; self; end
end

class Counter
  attr_accessor :limit
  def initialize(limit); @limit = limit; end
  def run(a, at)
    i = 0
    s = 0
    w = Lowering.new(self, at)
    while i < @limit
      s += a[i]
      w += 1
      i += 1
    end
    [i, s]
  end
end

p Counter.new(3000).run(Array.new(5000, 1), 1)
p Counter.new(3000).run(Array.new(5000, 1), 1500)
p Counter.new(500).run(Array.new(5000, 1), 10_000)
