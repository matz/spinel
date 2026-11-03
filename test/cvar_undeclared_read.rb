# A read of a class variable that neither the class it is looked up from
# nor an ancestor or mixin declares raises NameError when it runs, naming
# the variable and that class.

class T
  def h = @@h
  def self.h = @@h
  def pick(flag) = flag ? @@h : :none
  begin
    p @@h
  rescue NameError => e
    p e.message
  end
end
begin
  T.new.h
rescue NameError => e
  p e.class, e.message, e.name, e.receiver
end
begin
  T.h
rescue NameError => e
  p e.message
end
p T.new.pick(false)

module Mod
  def self.h = @@h
end
begin
  Mod.h
rescue NameError => e
  p e.message
end

# another class's variable, and one only a subclass of the reader declares
class A
  @@a = 1
  def self.a = @@a
end
class B
  def a = @@a
  def twice = @@a * 2
end
class C < B
  @@a = 3
end
begin
  B.new.a
rescue NameError => e
  p e.message
end
begin
  C.new.twice
rescue NameError => e
  p e.message
end
p A.a

# a Struct block's variable is the enclosing class's, not the Struct
# subclass's
class Host
  S = Struct.new(:a) do
    @@s = 1
  end
end
class Kid < Host::S
  def s = @@s
end
begin
  Kid.new(1).s
rescue NameError => e
  p e.message
end

# a module's method copied into a class reads the module's variables, not
# the includer's
module Reader
  def get = @@x
end
class Box
  @@x = 2
  include Reader
end
begin
  Box.new.get
rescue NameError => e
  p e.message
end

# class_variable_set on the implicit self of a class or module body declares
# the variable, and class_variable_get of an undeclared one names it
class Set1
  class_variable_set(:@@x, 4)
  def self.x = @@x
  def self.missing = class_variable_get(:@@nope)
end
module SetM
  class_variable_set(:@@y, 11)
  def y = @@y
end
class SetC
  include SetM
end
p Set1.x, SetC.new.y
begin
  Set1.missing
rescue NameError => e
  p e.message, e.name, e.receiver
end

# a bare class_variable_* in a class_eval block on another class acts on
# that class, written in a class body or in a class method
class EvalOther
  @@o = :other
end
class EvalHost
  @@o = :host
  EvalOther.class_eval { class_variable_set(:@@e, 8) }
  p(EvalOther.class_eval { class_variable_get(:@@o) })
  def self.m = EvalOther.class_eval { class_variable_set(:@@f, 15) }
end
EvalHost.m
p EvalOther.class_variable_get(:@@e), EvalHost.class_variable_defined?(:@@e)
p EvalOther.class_variable_defined?(:@@f), EvalHost.class_variable_defined?(:@@f)

# a program's own NameError with the same text has no name
begin
  raise NameError, "uninitialized class variable @@zz in Foo"
rescue NameError => e
  p e.name
end

# defined? and ||= do not raise
class D
  def self.dy = defined?(@@y)
  def self.n = (@@n ||= 4)
end
p D.dy, D.n

# an undeclared read stored into a typed class variable, global or
# argument raises before the store
class Typed
  @@ready = true
  @@i = 1
  def self.take(x) = x
  def self.go
    r = []
    begin; @@ready = @@missing; rescue NameError => e; r << e.name; end
    begin; @@i = @@missing; rescue NameError => e; r << e.name; end
    begin; $tg = 1; $tg = @@missing; rescue NameError => e; r << e.name; end
    begin; take(true); take(@@missing); rescue NameError => e; r << e.name; end
    r << @@ready << @@i << $tg
  end
end
p Typed.go

# a class_variable_set, bare or on self, in a class_eval block nested in
# another class's acts on the inner receiver, not on the outer one
class NestHost; end
class NestOther; end
NestHost.class_eval { NestOther.class_eval { class_variable_set(:@@o, 2) } }
NestHost.class_eval { NestOther.class_eval { self.class_variable_set(:@@q, 3) } }
p NestHost.class_variable_defined?(:@@o), NestHost.class_variable_defined?(:@@q)
p NestOther.class_variable_defined?(:@@o), NestOther.class_variable_defined?(:@@q)

# a String name, and a name that is no class variable's, given to the
# reflection on the implicit self of a class body
class Refl
  class_variable_set("@@s", 5)
  def self.s = @@s
  begin
    class_variable_set(:"@@x-y", 1)
  rescue NameError => e
    p e.message, e.name
  end
  p class_variable_get("@@s"), class_variable_defined?("@@s")
end
p Refl.s, Refl.class_variables

# class_variable_get of a declared variable that no write has set yet
class Later
  begin
    class_variable_get(:@@v)
  rescue NameError => e
    p e.message, e.name
  end
  @@v = 1
  p class_variable_get(:@@v)
  def self.peek
    Later.class_variable_get(:@@w)
  rescue NameError => e
    e.receiver
  end
  def self.set = (@@w = 2)
end
p Later.peek
Later.set
p Later.peek

# ...but a program's own instance_eval runs its block where it stands, and
# a block argument's expression runs with the class_eval block's self
class OwnEval
  def instance_eval(&b) = b.call
end
own = OwnEval.new
p(NestHost.class_eval { own.instance_eval { self } })
NestHost.class_eval { own.instance_eval { class_variable_set(:@@own, 1) } }
p NestHost.class_variable_defined?(:@@own)
def keep_self(x) = proc { x }
p(NestHost.class_eval { NestOther.instance_exec(&keep_self(self)) })
