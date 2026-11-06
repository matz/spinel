# A bare call (or `self.m`) in a superclass method whose own chain has no
# method of the name reaches the top-level def or Object's method. A
# subclass's own method of the name answers for an instance of the subclass.
def hi(a = 1) = "top#{a}"
class Foo
  def call_hi = hi
  def call_hi2 = hi(5)
end
class Sub < Foo; def hi(a = 2) = a * 10; end
class Sub2 < Foo; end
class Deep < Sub; end
module M; def hi(a = 3) = "m#{a}"; end
class Inc < Foo; include M; end
class Priv < Foo; private def hi(a = 4) = "priv#{a}"; end
p Sub.new.call_hi, Sub.new.call_hi2
p Foo.new.call_hi, Foo.new.call_hi2
p Sub2.new.call_hi, Deep.new.call_hi, Inc.new.call_hi, Priv.new.call_hi
p [Foo.new, Sub.new, Deep.new].map { |o| o.call_hi }

class Object; def greet = "obj"; end
class Base
  def g = greet
  def h = self.greet
  def k = "<#{greet}>"
end
class Num < Base; def greet = 42; end
class Sym < Base; def greet = :sym; end
[Base.new, Num.new, Sym.new].each { |o| p [o.g, o.h, o.k] }

def log_it = puts("top log")
class Job; def run; log_it; :done; end; end
class Loud < Job; def log_it = puts("loud log"); end
p Job.new.run, Loud.new.run

def val = 1
class K; def twice = val + val; end
class L < K; def val = 20; end
p K.new.twice, L.new.twice

class Err < StandardError; def m = hi; end
class Err2 < Err; def hi = "err2"; end
p Err2.new.m, Err.new.m
