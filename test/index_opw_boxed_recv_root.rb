# spinel: gc-stress
# `a[key] op= v` on a receiver held boxed reads the receiver into a temp,
# then the key. A key or right-hand side that runs code can rebind the
# variable the receiver came from -- through a closure over the local, or a
# method writing the instance variable -- and leave the temp the old
# object's only holder while it allocates. The temp was rooted only for a
# right-hand side with an effect: the read after the key took a freed Array
# ("undefined method '+' for nil"). The op-assign used as a value holds its
# receiver in a temp of its own, typed or boxed, and had the same gap.
def boxed(x) = x.respond_to?(:zzz) ? 0 : x

def by_closure
  a = boxed([1, 2, 3])
  swap = -> { a = Array.new(1000) { |i| i * 2 }; Array.new(1000) { |i| "s#{i}" }.size % 3 }
  a[swap.call] += 7
  [a.size, a.sum]
end
p by_closure

def by_closure_rhs
  a = boxed([1, 2, 3])
  swap = -> { a = Array.new(1000) { |i| i }; Array.new(1000) { |i| "r#{i}" }.size }
  a[1] += swap.call
  [a.size, a.sum]
end
p by_closure_rhs

class Hist
  def initialize
    @h = boxed([0, 0, 0])
  end
  def swap
    @h = boxed(Array.new(1000) { |i| "x#{i}" }.map(&:size))
    0
  end
  def run
    @h[swap] *= 3
    @h.sum
  end
end
p Hist.new.run

# a Hash: the String key's own temp is rooted apart
class Tally
  def initialize = @t = boxed({ "k" => 1 })
  def key = (@t = boxed({ "k" => 100 }); Array.new(1000) { |i| "t#{i}" }; "k")
  def run = (@t[key] += 1; @t)
end
p Tally.new.run

# a receiver nothing else holds, and a key that allocates
def fresh = boxed([5, 6, 7])
p (fresh[Array.new(1000) { |i| "f#{i}" }.size % 3] -= 2)

# a plain local nothing rebinds still holds its Array
def counts(s)
  c = boxed([0, 0, 0, 0])
  i = 0
  while i < s.size
    c[s.getbyte(i) % 4] += "#{i}".size
    i += 1
  end
  c
end
p counts("spinel" * 50)

# ...and the op-assign used as a value
def ints = [1, 2, 3]
def typed_val
  a = ints
  swap = -> { a = Array.new(1000) { |i| i * 2 }; Array.new(1000) { |i| "s#{i}" }.size % 3 }
  r = (a[swap.call] += 7)
  [r, a.size, a.sum]
end
p typed_val

def boxed_val
  a = boxed([1, 2, 3])
  swap = -> { a = Array.new(1000) { |i| i * 2 }; Array.new(1000) { |i| "s#{i}" }.size % 3 }
  r = (a[swap.call] += 7)
  [r, a.size, a.sum]
end
p boxed_val

class TallyVal
  def initialize = @t = boxed({ "k" => 1 })
  def key = (@t = boxed({ "k" => 100 }); Array.new(1000) { |i| "t#{i}" }; "k")
  def run = [(@t[key] += 1), @t]
end
p TallyVal.new.run

class HistVal
  def initialize = @h = [0, 0, 0]
  def swap = (@h = Array.new(1000) { |i| "x#{i}" }.map(&:size); 0)
  def run = [(@h[swap] *= 3), @h.sum]
end
p HistVal.new.run
