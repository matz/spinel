# v::Foo, where v holds a class or module at run time, reads the Foo that
# comes first in v's own ancestors: an included module's before the
# superclass's, the later of two includes, a prepend before the class, and
# an include written in a reopening.
class Foo; def who = "top"; end
class Base; class Foo; def who = "base"; end; end
module Inc; class Foo; def who = "inc"; end; end
module Inc2; class Foo; def who = "inc2"; end; end
class Kid3 < Base; include Inc; end
class Kid4 < Base; end
class Kid5 < Kid3; end
class Two; include Inc; include Inc2; end
class TwoB; include Inc2, Inc; end
class Kid6 < Kid3; include Inc2; end
class Own < Base; include Inc; class Foo; def who = "own"; end; end
module MM; include Inc; end
class ViaMM < Base; include MM; end
def f(v) = v::Foo.new.who
[Kid3, Kid4, Kid5, Two, TwoB, Kid6, Own, MM, ViaMM, Base, Inc, Inc2, Object].each { |k| puts "#{k}: #{f(k)}" }
begin; f(Comparable); rescue NameError => e; puts e.message; end
begin; f(3); rescue TypeError => e; puts e.message; end
k = Class.new(Kid3)
puts f(k)
def g(v) = (v::Foo rescue :err)
p g(Kid3), g(Kid4)

class BaseB; class Foo; def w = :base; end; end
module IncB; class Foo; def w = :inc; end; end
module ExtB; module Mix; class Foo; def w = :mix; end; end; end
class KidB < BaseB; include IncB; end
class KidB2 < BaseB; include ExtB::Mix; end
class KidB3 < BaseB; end
class KidB3; include IncB; end
module PreB; class Foo; def w = :pre; end; end
class KidB4 < BaseB; include IncB; prepend PreB; end
class KidB5 < BaseB; include Comparable; include IncB; end
def fb(v) = v::Foo.new.w
[KidB, KidB2, KidB3, KidB4, KidB5, BaseB, IncB, PreB].each { |k| p [k, fb(k)] }

# A diamond: a module two mixins bring sits where the first include put
# it. A class's own Foo comes before its prepend's; a subclass reads the
# prepend's. An include in a reopening of the holder comes after its own.
module DC; class Foo; def w = :c; end; end
module DA; include DC; class Foo; def w = :a; end; end
module DB; include DC; end
class DK; include DA; include DB; end
module DPre; class Foo; def w = :pre; end; end
class DOwn < BaseB; prepend DPre; class Foo; def w = :own; end; end
class DOwn2 < DOwn; end
module DMM; include IncB; end
module DInc2; class Foo; def w = :inc2; end; end
class DDup; include DMM; include DInc2; include IncB; end
module DIncA; class Foo; def w = :inca; end; end
class BaseB; include DIncA; end
class DUnder < BaseB; end
p [fb(DK), fb(DOwn), fb(DOwn2), fb(DDup), fb(BaseB), fb(DUnder)]

# The insertion point of an include moves only forward; an unrelated
# class's own `include` method does not change how class bodies read.
module IA; end
module IB; class Foo; def w = :b; end; end
module IN; class Foo; def w = :n; end; end
module IM; include IN; include IA; include IB; end
class IK; include IB; include IA; include IM; end
class Builder; def include(x) = x; end
p [fb(IK), Builder.new.include(3)]
