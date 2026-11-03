# module_function copies what CRuby copies to a module's singleton: a def, a
# define_method and `module_function :n` of a name, which for an alias is the
# method the alias captured (an included module's too) and not a later def of
# the alias's name. An alias, an attr reader and an undef under the bare word
# copy nothing, and an alias of a module function is no module function.
# A module function is also the module's private instance method: the
# latest def of its name, which an includer and an instance alias reach.

module W
  module_function
  def a = 1
  alias b a
  alias_method :c, :a
  define_method(:dm) { 7 }
  def z = 2
end
p W.a, W.dm, W.z
p W.respond_to?(:b), W.respond_to?(:c)
p W.singleton_methods.sort
p W.private_instance_methods(false).sort

module Base
  def greet = "hi"
end
module V
  include Base
  alias_method :hello, :greet
  module_function :hello
end
p V.hello
p V.respond_to?(:greet)

module X
  def m = 1
  alias n m
  module_function :n
  def n = 2
end
p X.n
include X
p n

module Y
  def a = 1
  module_function :a
  alias b a
end
p Y.respond_to?(:b)
p Y.singleton_methods.sort
include Y
p b

module R47
  def a = 1
  def a = 2
  module_function :a
end
class C47; include R47; def t = a; end
p R47.a, C47.new.t

module R49
  def a = 1
end
module R49
  def a = 2
  module_function :a
end
class C49; include R49; def t = a; end
p R49.a, C49.new.t

module R45
  def a = 1
  alias b a
  module_function :b
  def a = 2
  module_function :a
end
class C45; include R45; def t = [a, b]; end
p R45.a, R45.b, C45.new.t

module R50
  def a = 1
  alias b a
  def a = 2
  module_function :a
end
class C50; include R50; def t = [a, b]; end
p R50.a, C50.new.t

module R12
  def a = 1
  module_function :a
  alias b a
end
class C12; include R12; def t = b; def u = a; end
p C12.new.t, C12.new.u

module DM
  module_function
  define_method(:dm) { 7 }
end
class CDM; include DM; def t = dm; end
p CDM.new.t, CDM.new.send(:dm), DM.dm

module G0
  def h(x) = x * 1
  alias g h
  module_function :g
end
module G1
  def h(x) = x * 2
  alias g h
  module_function :g
end
p G0.g(1), G1.g(2)
p R12.instance_method(:a).owner, R12.method(:a).owner
