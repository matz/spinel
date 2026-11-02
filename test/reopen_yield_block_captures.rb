# A block handed to a reopened builtin's yielding method writes and reads
# the enclosing locals themselves, not copies.
class Float
  def twice
    yield 1
    yield 2
    self
  end
end

class Symbol
  def once
    yield to_s
  end
end

class Array
  def each_pair_of
    yield first, last
    nil
  end
end

class Hash
  def each_key_once
    each_key { |k| yield k }
    self
  end
end

class Numeric
  def upto_once
    yield self + 1
  end
end

class Object
  def with_me
    yield self
  end
end

module Kernel
  def kernel_once
    yield 7
  end
end

total = 0
1.5.twice { |v| total += v }
p total

name = nil
:sym.once { |s| name = s.upcase }
p name

acc = []
[3, 4, 5].each_pair_of { |a, b| acc << a << b }
p acc

keys = []
{ x: 1, y: 2 }.each_key_once { |k| keys << k }
p keys

n = 10
3.upto_once { |v| n += v }
p n

seen = []
42.with_me { |o| seen << o }
"str".with_me { |o| seen << o }
p seen

hits = 0
5.kernel_once { |v| hits = v }
p hits

def in_method
  sum = 0
  [1, 2].each_pair_of { |a, b| sum = a + b }
  2.5.twice { |v| sum += v }
  sum
end
p in_method
