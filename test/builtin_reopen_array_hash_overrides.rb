# An Array or Hash reopen that defines a builtin's name owns it, as the
# scalar reopens do (builtin_reopen_overrides.rb): `class Array; def first
# = "arr"; end` makes `[1, 2].first` answer "arr". The reopened method was
# emitted and reached only for a name no builtin arm takes, so an override
# was ignored and the builtin answered, typed as the builtin's element.

class Array
  def first = "arr-first"
  def sort = :sorted
  def last(n = 1) = n * 10
end

class Hash
  def size = "hash-size"
end

class Pt
  def initialize(x) = @x = x
end

p [1, 2].first
p ["a", "b"].first
p [1.5].first
p [Pt.new(1)].first
p [3, 1].sort
p [1, 2].last
p [1, 2].last(3)
p({a: 1}.size)
p({"k" => 2.5}.size)

# a builtin name the reopens leave alone still answers the builtin
p [3, 1].max
p({a: 1}.keys)

# an Object reopen does not displace Array's own builtin
class Object
  def max = "object-max"
end
p [3, 1].max

# A yield receiver whose block answers an Integer at one site and a Float at
# another runs each site's own method; a reopen that answers another type
# for one of them is boxed as that type, not as the builtin's Integer.
class Integer
  def abs = "int-abs"
end
class Float
  def -@ = :negated
end
def absolute = yield.abs
p absolute { -3 }
p absolute { -2.5 }
def negate = yield.-@
p negate { 5 }
p negate { 2.5 }

# An alias taken before the reopen keeps naming the builtin; one taken after
# names the reopen's method, as in CRuby.
class Array
  alias orig_sum sum
  def sum = "arr-sum"
  alias my_sum sum
end
class Hash
  alias orig_values values
  def values = "hash-values"
end
p [1, 2].orig_sum
p [1, 2].sum
p [1, 2].my_sum
p({a: 1}.orig_values)
p({a: 1}.values)
