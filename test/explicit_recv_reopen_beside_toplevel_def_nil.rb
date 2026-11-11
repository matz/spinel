# A call on a builtin value of a method the program defines on the
# value's class answers that method's value, nil included (a yielding
# one's block value), beside a top-level def of the same name: the
# top-level def is private, so it is no target of a call with a receiver.
# `self.f` still reaches it, and a call on a value whose class has no
# method of the name raises.
def f
  1
end

class NilClass
  def f
    yield
  end
end

class String
  def f
    yield
  end
end

class TrueClass
  def f = yield
end

class FalseClass
  def f = yield
end

class Float
  def f(k) = k > 0 ? 7 : nil
end

def pick(k) = k > 0 ? 7 : nil

[1, 0].each do |k|
  x = nil.f { pick(k) }
  p x
  p x.nil?
  y = "s".f { pick(k) }
  p(y.nil? ? :none : y + 1)
  t = true.f { pick(k) }
  p(t.nil? ? :none : t + 2)
  u = false.f { pick(k) }
  p u
  w = 2.5.f(k)
  p(w.nil? ? :none : w + 3)
end

def g(k) = k > 0 ? 3 : nil

[1, 0].each do |k|
  z = self.g(k)
  p(z.nil? ? :none : z * 2)
end

p(nil.f { 4 } * 2)

def h(k) = k > 0 ? 7 : nil
begin
  p 5.h(0)
rescue NoMethodError => e
  p e.class
end
