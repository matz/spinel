# A yielding method reached only through a zero-argument poly call. Its
# proc-form clone's parameters are poly from the start, but the re-narrow
# reset cleared them every round and no call site widened them again, so
# the clone read its boxed argument as an sp_int and the C did not build.
# A class whose method needs arguments also had no dispatch arm for the
# zero-argument call, so it raised NoMethodError instead of ArgumentError.

# 1. the reported shape
class A
  def w(x) = yield(x)
end
class B
  def w = yield(:b)
end
class C
  def w(x, y) = yield(x, y)
end
[A.new, B.new, C.new].each do |o|
  p o.w { |v| [:blk, v] }
rescue ArgumentError => e
  p e.message
end

# 2. optional and rest parameters word the count as CRuby does
class D
  def v(x, y = 1) = yield(x + y)
end
class E
  def v(x, *r) = yield(x, r)
end
class F
  def v = yield(:f)
end
[D.new, E.new, F.new].each do |o|
  p o.v { |a| a }
rescue ArgumentError => e
  p e.message
end

# 3. no block: a zero-argument poly call to a method that takes one
class G
  def u(x) = x
end
class H
  def u = :h
end
[G.new, H.new].each do |o|
  p o.u
rescue ArgumentError => e
  p e.message
end
