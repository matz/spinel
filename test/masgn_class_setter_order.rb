# A multiple assignment with a setter on a class writes its targets left
# to right, as CRuby does: a target after the class setter is written
# after the setter runs, and the setter sees the old value.

$g = :old
class K
  def self.a=(v); puts "K.a=#{v.inspect} sees $g=#{$g.inspect} @@c=#{@@c.inspect}"; end
  def self.b=(v); puts "K.b=#{v.inspect}"; end
  @@c = :old
  def self.swap
    K.a, @@c = 1, :new
    p @@c
  end
end
K.a, $g = 1, :new
p $g

class O
  def initialize(t) = @t = t
  def b=(v); puts "#{@t}.b=#{v.inspect}"; end
end
o = O.new("o")
K.b, o.b = 1, 2
o.b, K.b = 3, 4

K.swap

class H
  def self.x=(v); puts "H.x=#{v.inspect} sees @y=#{H.y.inspect}"; end
  class << self; attr_accessor :y; end
  def self.run
    H.x, self.y = 5, 6
    p y
  end
end
H.y = 0
H.run

# a rest after the class setter, and locals and a later class setter
K.a, *r = 7, 8, 9
p r
x = 0
K.b, x, K.a = 10, 11, 12
p x
first, K.b, last = :f, :m, :l
p [first, last]

def m
  $g = :in_m
  K.a, $g = :v, :w
  p $g
end
m

class Box
  def []=(k, v); puts "box[#{k.inspect}]=#{v.inspect}"; end
end
box = Box.new
key = :k
K.b, box[key], box[1] = :p, :q, :r
h = {}
K.b, h[:z] = 1, 2
p h

# a later target's receiver and key are read before the values, as CRuby
# reads them, also when another target or a lambda writes them
o = O.new("o")
K.b, o, o.b = 1, O.new("p"), 3
o = O.new("o")
o, K.b, o.b = O.new("q"), 1, 3
key = :k
K.b, key, box[key] = 1, :n, 2
o = O.new("o")
f = -> { o = O.new("r"); 1 }
K.b, o.b = f.(), 2

class O
  def run
    K.b, self.c = 1, 2
    nil
  end
  private def c=(v); puts "private c=#{v.inspect}"; end
end
O.new("s").run

# a rest that cannot move (a setter on a computed receiver) stays in the
# assignment, and the class setters still move
class Keep; attr_accessor :v; end
def mk = (@mk ||= Keep.new)
K.b, *mk.v = 1, 2, 3
p mk.v
