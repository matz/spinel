class A
  attr_reader :v
  def initialize(x) = @v = [:a, x]
end
class B < A; end
class C < B; end
p C.new(1).v
class B
  def self.new(x) = super(x + 100)
end
p C.new(2).v
p A.new(3).v
