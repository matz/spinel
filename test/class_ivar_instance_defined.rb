# A class's own ivars (class body, class methods) are not its instances':
# the instance's reflection does not see them, and the class keeps them.
class A
  @made = 0
  @label = "a"
  def self.made = @made
  def self.label = @label
  def initialize(a) = @a = a
  attr_reader :a
end
a = A.new(1)
p a.instance_variables
p a.instance_variable_defined?(:@made)
p a.instance_variable_defined?(:@a)
p a.instance_variable_get(:@made)
p A.made
p A.label
p a.a
