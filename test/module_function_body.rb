# module_function in a method body makes the method private on the module's
# instance side and copies it to the module's singleton once the word runs:
# the copy is the def live at the word (an earlier def of the same body's,
# else the one live where the body runs). The word answers its argument as
# given and raises CRuby's errors, making the names ahead of the raise private
# and copying none: NameError for a name the module does not define, TypeError
# for an element of an Array that is no name, then TypeError for the Array. In
# a class, which has no module_function, it raises NoMethodError and changes
# nothing. A Method of the copy is there once the word has run.
module M
  def a = :a
  def b = :b
  def c = 3
  def self.one = (module_function(:a); nil)
  def self.two
    module_function :b, :c
  end
  def self.arr
    module_function [:a, :b]
  end
  def self.str = module_function("c")
  def self.missing = (module_function :zz rescue [$!.class, $!.message])
  def d = :d
  def self.part = (module_function :d, :zz rescue $!.class)
end
class KC
  def x = :x
  def self.mf = (module_function :x rescue $!.class)
end
# the copy is the def live where the word runs, not one a later reopening makes
module V
  def v = :v1
  def self.go = (module_function :v; nil)
end
class KV; include V; end
class K; include M; end
# the copy is the def live where the word runs
module R
  def m = :m1
  def self.redef
    def m = :m2
    module_function :m
    nil
  end
end
# a class that extends the module keeps its copy, made private
module E
  def e = :e
  def self.hide = (module_function :e; nil)
end
class X; extend E; end
# a Method of the copy raises NameError until the word has run, naming the
# module's singleton class once something made it, Module before
module C
  def c = :c
  def self.hide = (module_function :c; nil)
end
module L
  def l = :l
end
lm = -> { module L; module_function :l; end; nil }

def lists = p([M.singleton_methods.sort, M.private_instance_methods(false).sort, M.public_method_defined?(:a)])
def caps = p([(C.method(:c).call rescue [$!.class, $!.message]), (L.method(:l).call rescue [$!.class, $!.message])])
def chk_e(t) = p([t, (E.e rescue $!.class), (X.e rescue $!.class), X.respond_to?(:e), X.respond_to?(:e, true)])
k = K.new
lists
p [(M.a rescue $!.class), M.respond_to?(:a), (k.a rescue $!.class)]
p M.one
p [M.a, M.respond_to?(:b), (k.a rescue $!.class), k.b, k.respond_to?(:a, true)]
r = M.two
p [r, M.b, M.c + 1, (k.b rescue $!.class), (k.c rescue $!.class)]
p((M.arr rescue [$!.class, $!.message]))
p [M.str, M.missing]
lists
p [M.part, (k.d rescue $!.class), M.respond_to?(:d)]
p [KC.mf, KC.new.x, KC.respond_to?(:x)]
V.go
module V; def v = :v2; end
p [V.v, KV.new.v]
R.redef
p [R.m, R.private_method_defined?(:m)]
chk_e(:before)
E.hide
chk_e(:after)

caps
C.hide
lm.call
caps

# in a module body too: an Array's names go private, then TypeError, with no
# copy; a String is answered as given, and a name the module does not define
# raises NameError
module B
  def x = :x
  def y = :y
  p((module_function [:x] rescue [$!.class, $!.message]))
  p module_function("y")
  p((module_function :zz rescue [$!.class, $!.message]))
  def g = :g
  def h = :h
  NM = 1
  module_function [:g, NM] rescue p($!.message)
  module_function [:h, "g"] rescue p($!.message)
end
class KB; include B; end
p [B.respond_to?(:x), B.y, B.private_method_defined?(:x)]
p [(KB.new.g rescue $!.class), (KB.new.h rescue $!.class), B.respond_to?(:g), B.respond_to?(:h)]
# an alias the module has counts as its method, as a def does
module W
  def ws(l) = l.split(" ")
  alias wa ws
  module_function :ws, :wa
  class << self
    alias split ws
  end
end
p [W.split("a b"), W.wa("c d")]
# defs of other names between a body's def and its word leave that def the live
# one; so does any other statement that runs no other body's def of the name, though
# one that does is called later (a statement that may run it is refused,
# test/reject/module_function_body_call_between.rb)
module D
  def self.setup
    def d1 = 1
    def d2 = 2
    module_function :d1
    nil
  end
end
D.setup
p [D.d1, D.respond_to?(:d2)]
module D2
  def self.change = (def d3 = 9)
  def self.setup
    def d3 = 3
    p 1
    module_function :d3
    nil
  end
end
D2.setup
D2.change
p [D2.d3, D2.instance_method(:d3).owner]
