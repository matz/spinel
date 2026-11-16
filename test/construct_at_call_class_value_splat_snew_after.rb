class A
  attr_reader :v
  def initialize(*a) = @v = a.sum
end
class B
  attr_reader :v
  def initialize(*a) = @v = a.size
end
args = [1, 2, 3]
[A, B].each { |k| p k.new(*args).v }
class A
  def self.new(*a) = super(*a, 100)
end
[A, B].each { |k| p k.new(*args).v }
