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
