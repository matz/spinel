# Where the class facts may not narrow a class value's `new` or a computed
# send on its instance, each still reaches the class CRuby reaches.

# a top-level ivar adds a class (its holder) after the facts are made: a
# Class object read from them still names the class it was made for
@d = :main
class K
end
class E < StandardError
end
k = [E][0]
p k.new.class

class Base
  def initialize(req) = @req = req
end
class Pair
  def initialize(a, b) = (@a = a; @b = b)
  def pair_only = "pair"
end

# a conjunct after the subclass test that rebinds the tested local
class Other
  def initialize(req) = @req = req
  def kind = "other"
end
class Sub < Base
  def kind = "sub"
end
def pick(name)
  klass = Object.const_get(name)
  klass < Base ? klass : nil
end
p pick("Sub").new(1).kind
def pick2(name)
  klass = Object.const_get(name)
  again = -> { klass = Other; true }
  klass < Base && again.call ? klass : nil
end
p pick2("Sub").new(2).kind
def pick3(name)
  klass = Object.const_get(name)
  again = -> { klass = Other; true }
  klass < Base && (again.call; true) ? klass : nil
end
p pick3("Sub").new(3).kind

# `new(...)` forwards two arguments to a two-argument initialize
def build(name, ...) = Object.const_get(name).new(...)
p build("Pair", 3, 4).public_send("pair_only".to_sym)
