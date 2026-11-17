class A
  attr_reader :v
  def initialize(x) = @v = [:a, x]
end
class B
  attr_reader :v
  def initialize(x) = @v = [:b, x]
end
[A, B].each { |k| p k.new(1).v }
class B
  def self.new(x) = super(x * 9)
end
[A, B].each { |k| p k.new(2).v }
