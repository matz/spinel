# A constant read that finds nothing raises NameError naming the class whose
# body is the read's lexical scope: the def's class for a method, whichever
# class calls it, and nothing at the top level. A builder's block and an
# eval form's block push no scope.
def t
  yield
rescue NameError => e
  p [e.message, e.name]
end

class Q
  def self.a = MISS1
  def b = MISS2
  def d = [1].map { MISS4 }
  module In; def self.e = MISS5; end
  class In2; def f = MISS6; end
  begin
    MISS7
  rescue NameError => e
    p [e.message, e.name]
  end
  def n = Nope::X
  def s = self.class::MISS8
end
class Sub < Q; end
module Mo
  def g = MISS9
  def self.h = MISS10
end
class Inc; include Mo; end
class Q::Comp; def j = MISS11; end

t { Q.a }
t { Sub.a }
t { Q.new.b }
t { Sub.new.b }
t { Q.new.d }
t { Q::In.e }
t { Q::In2.new.f }
t { Inc.new.g }
t { Mo.h }
t { Q::Comp.new.j }
t { Q.new.n }
t { Q.new.s }

t { MISS13 }
def top = MISS14
t { top }
t { Nope2::Y }

Q.class_eval do
  begin
    MISS15
  rescue NameError => e
    p [e.message, e.name]
  end
end
A1 = Class.new { def l = MISS16 }
t { A1.new.l }
S1 = Struct.new(:x) { def m = MISS17 }
t { S1.new(1).m }

# a constant the program assigns later reads the same way until then
class Early
  def self.r = LATER
end
t { Early.r }
LATER = 1
p Early.r
