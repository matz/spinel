# Two macro modules share a last name (A::ClassMethods, B::ClassMethods): an
# extend of one cannot say which, so the macro call is left as written (a
# run-time public_send, refused), not expanded with the other's macro.
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
  extend B::ClassMethods
  def self.a_x = 1
  def self.b_x = 2
  constant :X
end
p Calc::X
