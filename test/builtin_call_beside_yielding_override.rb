# A call that is the builtin's own method, a `super` out of a builtin
# subclass's override or an alias made of the builtin before the override, runs
# the builtin whatever the program defines under the same name: a yielding
# method on Object, a yielding override on the same subclass or one below it,
# a private neighbour. The block such a call carries is the caller's block,
# typed by the builtin's iteration, not by the program's method's `yield`.
class Object
  def each_with_index(*a) = yield(:obj)
end

class IndexArray < Array
  def each_with_index(*a, &b)
    super(*a, &b)
  end
end
IndexArray[1, 2, 3].each_with_index { |*x| p x }

class Doubling < Array
  def map
    return yield(:x) if false
    super { |v| v * 2 }
  end
end
p Doubling[1, 2].map

class Reversing < Doubling
  def map
    return yield(:y) if false
    super.reverse
  end
end
p Reversing[1, 2, 3].map

class Odd < Array
  def select
    return yield(:x) if false
    super { |v| v.odd? }
  end
  private def each_slice(n) = yield(:priv)
end
p Odd[1, 2, 3].select

class Big < Hash
  def select
    return yield(:x, :y) if false
    super { |k, v| v > 1 }
  end
end
h = Big.new
h[:a] = 1
h[:b] = 2
p h.select

class Loud < String
  def each_char
    return yield(:x) if false
    out = []
    super { |ch| out << ch.upcase }
    out
  end
end
p Loud.new("ab").each_char

class Forward < Array
  def map(&blk)
    return yield(:x) if false
    super(&blk)
  end
end
p Forward[1, 2].map { |v| v + 100 }

class Captured < Array
  alias old_map map
  def map = yield(:mine)
end
p Captured[1, 2].old_map { |v| v + 1 }
p Captured[1].map { |v| v }
