# spinel: gc-stress
# A default that reads a constant or an instance variable is spelled where the
# call is, and a later default of the same call runs code that rebinds what it
# read: the read still comes first, as in CRuby. A call that fills two or more
# defaults, one of which runs code, binds them in order. Each shape is a
# different way to call: a top-level method, keyword defaults, class and
# module methods, a constructor, `super(...)` and a top-level instance
# variable; the last two check that a default the call is given is not one it
# fills.
X = [1, 2, 3]
def swap
  Object.const_set(:X, [9])
  Array.new(200) { |i| "g#{i}" }
end
def m(a = X, b = swap)
  p a
  p b.size
end
m
m

X = [1, 2, 3]
def mk(a: X, b: swap)
  p a
  p b.size
end
mk
mk

X = [1, 2, 3]
class K
  def self.rebind
    Object.const_set(:X, [3])
    Array.new(100) { |i| "g#{i}" }
  end
  def self.m(a = X, b = rebind)
    p a
    p b.size
  end
end
K.m
K.m

module M
  T = [1]
  def self.re
    M.const_set(:T, [2])
    Array.new(100) { |i| "g#{i}" }
  end
  def self.m(a = T, b = re)
    p a
    p b.size
  end
end
M.m

X = [1, 2, 3]
class C
  def initialize(a = X, b = swap)
    p a
    p b.size
  end
end
C.new
C.new

X = [1, 2, 3]
class P
  def m(a = X, b = swap)
    p a
    p b.size
  end
end
class Q < P
  def m
    super()
  end
end
Q.new.m

@x = [1, 2, 3]
def bump
  @x = [7]
  Array.new(100) { |i| "g#{i}" }
end
def tm(a = @x, b = bump)
  p a
  p b.size
end
tm
tm

# a default the call is given is not filled: one default left, no ordering
X = [1, 2, 3]
def one(a = X, b = swap)
  p a
  p b.size
end
one([5])
one([5], [6, 7])
def onek(a: X, b: swap)
  p a
  p b.size
end
onek(b: [8])
onek(a: [4], b: [8, 9])

# defaults that log run in order, after the arguments the call gives
$log = []
def f(t) = ($log << t; Array.new(20) { t.to_s })
def lg(a = f(:a), b = $log.size, c = f(:c), k: f(:k), j: $log.dup)
  p [a.size, b, c.size, k.size, j]
end
lg
lg(k: [1])
lg([1], j: 0)   # (an Array: Integer#size is the word, 4 on a 32-bit target)
p $log

# a keyword the call gives runs before the default it leaves out reads
X = [1, 2, 3]
def swap5 = (Object.const_set(:X, [X.size + 6]); 5)
def swapa = (Object.const_set(:X, [X.size + 6]); [7])
def kg(a = X, k:) = p([a, k])
kg(k: swap5)
kg(k: swapa)
