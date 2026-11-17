D = Data.define(:x, :y)
p D.new(1, 2)
class D
  def initialize(x:, y: 5) = super(x: x * 2, y: y)
end
p D.new(1, 2)
p D.new(3)
