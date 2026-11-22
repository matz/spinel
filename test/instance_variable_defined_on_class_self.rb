# instance_variable_defined? on the class itself: in an included hook on
# the hook's parameter and with no receiver in its class_eval block, and
# in a class body with and without self.
module IvdFlags
  def self.included(base)
    p base.instance_variable_defined?(:@flag), base.instance_variable_defined?(:@none)
    base.instance_variable_set(:@late, 1) unless base.instance_variable_defined?(:@late)
    base.class_eval do
      @seen = instance_variable_defined?(:@flag)
      p instance_variable_defined?(:@seen)
    end
  end
end
class IvdA
  @flag = true
  include IvdFlags
  def self.late = @late
  def self.seen = @seen
end
class IvdB
  @late = 9
  include IvdFlags
  def self.late = @late
  def self.seen = @seen
end
p IvdA.late, IvdA.seen, IvdB.late, IvdB.seen
class IvdC
  @x = 1
  p instance_variable_defined?(:@x), instance_variable_defined?(:@y), self.instance_variable_defined?(:@x)
end
