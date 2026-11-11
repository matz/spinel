# A forwarding def of a module, copied onto an object by extend, is topped up
# with parameters the copy did not have; the copy's parameter lists must have
# room for them whatever their length.
class Base
  def check(a, b = 2, k: 3) = [a, b, k]
  def other = :other
end
class K < Base; end
module Wrap
  def check(...) = super
  def tag = :wrapped
end
o = K.new
o.extend(Wrap)
p o.tag
p o.other
p K.new.check(1, k: 5)
@o = K.new
@o.extend(Wrap)
p @o.tag
