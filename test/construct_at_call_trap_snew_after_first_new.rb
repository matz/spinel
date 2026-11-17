class K
  attr_reader :v
  def initialize(x) = @v = x
end
p K.new(1).v
class K
  def self.new(x) = super(x * 10)
end
p K.new(2).v
