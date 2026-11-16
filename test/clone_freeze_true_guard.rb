# clone(freeze: true) freezes the copy, so a later ivar write through
# the copy (an attr writer, a method, a Struct setter) raises FrozenError.
class Base; attr_accessor :d; def setd; @d = 1; end; end
class Kid < Base; end
def try(x) = (begin; x.setd; :no_error; rescue => e; e.class; end)
p try(Kid.new.clone(freeze: true))
arr = [Base.new, "s"]
p try(arr[0].clone(freeze: true))
h = {freeze: true}
p try(Base.new.clone(**h))
p try(Base.new.clone(freeze: false))
p try(Base.new.clone)
class Pt; def initialize(x) = @x = x; def bump = @x += 1; end
q = Pt.new(1).clone(freeze: true)
p((q.bump rescue $!.class))
S = Struct.new(:a)
s = S.new(1).clone(freeze: true)
begin; s.a = 2; p :no_error; rescue => e; p e.class; end
b = Base.new.clone(freeze: true)
begin; b.d = 3; p :no_error; rescue => e; p e.class; end
