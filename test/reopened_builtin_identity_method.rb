# A program that reopens a builtin class with freeze, dup, clone or itself
# gets that method's value from the call. Inference answered the receiver's
# own kind first (these four are the identity methods), so a String slot
# was read where the method answered a Symbol: a chained `.class` or `.frozen?`
# was emitted on the wrong type, and the call printed nil.

class String
  def freeze = :mine
  def dup = :d
  def itself = :i
  def clone(freeze: nil) = [:c, freeze]
end

def id(v) = v

s = +"ab"
p s.freeze, s.dup, s.itself
p s.clone, s.clone(freeze: true)
p s.freeze.class, s.dup.class, s.itself.class

# through a chain and a method result, the receiver a bang call's answer
t = +"cd"
p t.upcase!.freeze
u = +"ef"
p id(u).upcase!.freeze
p id(u).upcase.freeze
p id(u).dup
p id(u).itself

# the method's value used as a value
def seal(x) = x.freeze
p seal(+"q"), seal(+"q").class
a = [s.freeze, s.dup]
p a

# the scalar kinds
class Float; def freeze = :fl; end
class Integer; def dup = :i; end
class Symbol; def itself = :sy; end
class Range; def dup = :r; end
x = 1.5
p x.freeze, x.freeze.class, 3.dup, 3.dup.class, :a.itself, (1..2).dup, (1..2).dup.class

# the Array and the Hash keep working
class Array; def freeze = :af; end
class Hash; def dup = :hd; end
p [1].freeze, {a: 1}.dup

# a subclass keeps its own method over the reopening's
class MyS < String; def freeze = :x; end
p MyS.new("b").freeze
