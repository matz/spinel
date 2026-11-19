# Definition hooks that class method bodies define fire only once the body
# has run, the core's no-op before; a body's own def fires
# singleton_method_added on each run of the body.
class LaterA
  def self.install
    def self.method_added(n) = puts("A #{n}")
  end
  def a; end
  install
  def b; end
end

class LaterK
  def self.install
    def LaterK.method_added(n) = puts("K #{n}")
  end
  def a; end
  install
  def b; end
end

class LaterS
  def self.install
    def self.singleton_method_added(n) = puts("S #{n}")
  end
  def self.a; end
  install
  def self.b; end
end

class LaterC
  class << self
    def install
      def method_added(n) = puts("C #{n}")
    end
  end
  def a; end
  install
  def b; end
end

module MixM
  def self.setup
    def self.included(b) = puts("inc #{b}")
  end
end
class Inc0; include MixM; end
MixM.setup
class Inc1; include MixM; end

class Base
  def self.setup
    def self.inherited(k) = puts("inh #{k}")
  end
end
class Sub0 < Base; end
Base.setup
class Sub1 < Base; end
class Never
  def self.install
    def self.method_added(n) = puts("never #{n}")
  end
  def a; end
end

class Twice
  def self.singleton_method_added(n) = puts("tw #{n}")
  def self.install
    def self.made = :made
  end
  install
  install
  def self.after; end
end

class Seen
  def self.watch
    def self.method_added(n) = puts("seen #{n}")
  end
  def before; end
  watch
  def mid; end
  watch
  def after; end
end
p Twice.made

# a hook that its own body's def observes, run twice
class Self2
  def self.go
    def self.singleton_method_added(n) = puts("s2 #{n}")
    def self.extra = 1
  end
  go
  go
end

# two calls of the body in one statement, and a hook read as a value
class Pair
  def self.set
    def self.method_added(n) = puts("pair #{n}")
    1
  end
  p [set, set]
  def z; end
end
class Parent
  def self.install
    def self.method_added(n) = puts("parent #{n} on #{self}")
  end
end
class Child < Parent
  def early; end
end
Parent.install
class Child
  def late; end
end

class Left
  def self.install
    def Left.inherited(k) = puts("left got #{k}")
  end
end
class Right
  def self.install
    class << self
      def inherited(k) = puts("right got #{k}")
    end
  end
end
class L0 < Left; end
class R0 < Right; end
Left.install
Right.install
class L1 < Left; end
class R1 < Right; end

module Mx
  def self.arm
    def self.included(b) = puts("mx in #{b}")
    def self.extended(b) = puts("mx ext #{b}")
  end
end
class E0; extend Mx; end
k = ARGV.size == 0 ? Mx : nil
k&.arm
class E1; include Mx; extend Mx; end

class Boxed
  def self.install
    def self.method_added(n) = puts("boxed #{n}")
  end
end
v = [Boxed, 1][ARGV.size]
v.install
class Boxed
  def q; end
end
# The hook runs after the def, and the def's name stays the value of the
# code the def ends.
class Tail
  def self.singleton_method_added(x) = puts("tail #{x}")
  def self.last; def self.a = 1; end
  def self.arm(c)
    if c
      def self.b = 2
    else
      def self.c = 3
    end
  end
  def self.guarded
    begin
      def self.d = 4
    ensure
      puts "ensured"
    end
  end
end
p Tail.last
p Tail.arm(true), Tail.arm(false)
p Tail.guarded
p(-> { def Tail.e = 5 }.call)

# Before a body defines a class's hook, a send reaches the core's
# private no-op, and a public call raises.
class Late
  def self.install; def self.inherited(k) = :mine; end
end
p Late.send(:inherited, 1)
p (Late.inherited(1) rescue $!.class)
Late.install
p Late.inherited(1)

# A hook that reads a class ivar sees the subclass's, which a class
# method's write sets only where it runs.
class Abs
  def self.abstract! = @abstract = true
  def self.abstract? = @abstract
  abstract!
  def self.inherited(k)
    p k.instance_variable_defined?(:@abstract)
    super
  end
end
class AbsApp < Abs; abstract!; end
class AbsPosts < AbsApp; end
p Abs.abstract?, AbsApp.abstract?, AbsPosts.abstract?
p AbsApp.instance_variable_defined?(:@abstract), AbsPosts.instance_variable_defined?(:@abstract)

# A subclass's hook calls super into the hook a body defines on its
# superclass only once that body has run.
class SupA
  def self.install; def self.inherited(k); puts "SupA.inh #{k}"; end; end
end
class SupB < SupA
  def self.inherited(k); puts "SupB.inh #{k}"; super; end
end
class SupD < SupB; end
SupA.install
class SupE < SupB; end

# A def with a receiver in a class body a lambda opens is a statement of that body, and the hook of
# the receiver's class fires each time the lambda runs.
class HookRecv
  def self.singleton_method_added(n) = puts("HookRecv.added #{n}")
end
opens = -> { class HookBox; def HookRecv.made = :ok; end }
opens.call
opens.call
p HookRecv.made

puts :end
