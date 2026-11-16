class A
  attr_reader :v
  def initialize(x) = @v = x
end
class B
  attr_reader :v
  def initialize(x) = @v = -x
end
h = { a: A, b: B }
p h.map { |_, c| c.new(3).v }
class A
  def self.new(x) = super(x * 1000)
end
p h.map { |_, c| c.new(3).v }
