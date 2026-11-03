# `K.instance_method(:x).bind_call(obj)` where K answers x by an attr
# reader, writer or accessor reads or writes obj's slot, as the call of a
# def of x would: on a class constant, on a Class value only the run time
# knows, through a superclass's reader, a module's reader its includer
# takes, and a private one. A subclass's reader answers ahead of a def above
# it, and a def below a reader ahead of the reader. A writer's value widens
# the slot and is the call's answer. obj must be an instance of the owner,
# else TypeError, and a class with no x raises NameError (a module's
# reader is refused on a Class value, as a module's def is). The accessors
# stay out of the class's method lists. The name may be a String, and one
# declaration may list any number of names. Such a call raised NameError.

class Pt
  attr_reader :d, :e
  attr_accessor :w
  def initialize = (@d = 7; @e = nil; @w = 1)
  def bump = (@d += 1)
end
class Sub < Pt
end
class Base
  def d = "base"
  def initialize = (@x = 0)
  def bump = (@x += 1)
end
class Kid < Base
  attr_reader :d
  def initialize = (@d = "kid"; @x = 0)
end
class Over < Pt
  def d = "over"
end
class Priv
  private attr_reader :s
  def initialize = (@s = :sec)
  def bump = (@s = :x)
end
module Named
  attr_reader :nm
end
class Mx
  include Named
  def initialize = (@nm = "mx")
  def bump = (@nm += "!")
end
class Ot
  def lb = @lb
  def initialize = (@lb = "ot")
  def bump = (@lb += "!")
end
class Tag
  attr_reader :lb
  def initialize = (@lb = "tag")
  def bump = (@lb += "!")
end
class Plain
  def initialize = (@q = 0)
  def bump = (@q += 1)
end
class Nine
  attr_reader :a, :b, :c, :d, :e, :f, :g, :h, :i
  def initialize = (@a = 1; @b = 2; @c = 3; @d = 4; @e = 5; @f = 6; @g = 7; @h = 8; @i = 9)
  def bump = (@a += 1)
end
class Wr
  attr_writer :d
  def initialize = (@d = 0)
  def get = @d
end

pt = Pt.new
p Pt.instance_method(:d).bind_call(pt)
p Pt.instance_method(:e).bind_call(pt)
p Pt.instance_method(:w).bind_call(pt)
p Pt.instance_method(:d).bind_call(Sub.new)
p Sub.instance_method(:d).bind_call(Sub.new)
p Kid.instance_method(:d).bind_call(Kid.new)
p Base.instance_method(:d).bind_call(Kid.new)
p Over.instance_method(:d).bind_call(Over.new)
p Pt.instance_method(:d).bind_call(Over.new)
p Priv.instance_method(:s).bind_call(Priv.new)
p Mx.instance_method(:nm).bind_call(Mx.new)
p Pt.instance_method(:w=).bind_call(pt, "now a string")
p pt.w
p Pt.instance_method(:w).bind_call(pt)
begin
  Pt.instance_method(:d).bind_call(Base.new)
rescue TypeError => e
  p e.message
end

def go(k) = k.instance_method(:lb).bind_call(k.new)
def set(k, o, v) = k.instance_method(:d=).bind_call(o, v)
p go(Ot)
p go(Tag)
begin
  go(Plain)
rescue NameError => e
  p e.message
end
w = Wr.new
p set(Wr, w, 4.5)
p w.get
p set(Wr, w, [1])
p w.get

p Pt.instance_method("d").bind_call(pt)
p go(Tag) == Tag.instance_method("lb").bind_call(Tag.new)
n = Nine.new
p Nine.instance_method(:a).bind_call(n), Nine.instance_method(:b).bind_call(n), Nine.instance_method(:c).bind_call(n),
  Nine.instance_method(:d).bind_call(n), Nine.instance_method(:e).bind_call(n), Nine.instance_method(:f).bind_call(n),
  Nine.instance_method(:g).bind_call(n), Nine.instance_method(:h).bind_call(n), Nine.instance_method(:i).bind_call(n)

p Pt.instance_methods(false).sort
p pt.respond_to?(:d), pt.methods.include?(:w=)
