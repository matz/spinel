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
