# An extended hook, written in the module's second opening, adds to the
# module's own ivar from each extender and defines a method on it.
module Reg
  def self.total = @total
end
module Reg
  @total = 10
  def self.extended(base)
    @total = @total + base.weight
    base.class_eval do
      def tag = "tagged"
    end
  end
end
class A
  def self.weight = 5
  extend Reg
end
class B
  def self.weight = 7
  extend Reg
end
p Reg.total, A.new.tag
