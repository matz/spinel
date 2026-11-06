# The text pass refuses this call (test/reject/macro_two_modules.rb); the
# class-body macro form runs the macro at the call's point under the load
# graph, as CRuby runs it.
module A
  module ClassMethods
    def constant(c) = const_set(c, public_send("a_#{c.to_s.downcase}"))
  end
end
module B
  module ClassMethods
    def constant(c) = const_set(c, public_send("b_#{c.to_s.downcase}"))
  end
end
class Calc
  extend A::ClassMethods
  extend B::ClassMethods
  def self.a_x = 1
  def self.b_x = 2
  constant :X
end
p Calc::X
