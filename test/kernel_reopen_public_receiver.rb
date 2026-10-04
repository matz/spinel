# A public method a `module Kernel` reopening defines is a method of every
# object: an explicit receiver reaches it as a bare call does. One under
# module_function stays private, and Kernel.m still calls it.
module Kernel
  def me = self
  def twice(x) = [self, x * 2]
end
p 5.me
p "s".me
p [1].me
p Object.new.me.class
p me.class
p nil.me
class Q
  def z = me
  def w = self.me
  def t = twice(3)
end
p Q.new.z.class, Q.new.w.class, Q.new.t[1]
p 4.twice(2)
p twice(1)[1]
module Kernel
  module_function
  def helper = :h
end
p helper
p Kernel.helper
p((5.helper rescue $!.class))
p RuntimeError.new("x").me
p((1..2).me)
p :s.me
