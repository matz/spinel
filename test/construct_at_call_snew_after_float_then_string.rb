class K
  attr_reader :v
  def initialize(x) = @v = x
end
p K.new(1.5).v
class K
  def self.new(n) = super(n.to_s)
end
p K.new(3).v
