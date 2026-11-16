class K
  attr_reader :v
  def initialize(x) = @v = x
end
k = [K, String][0]
p k.new(1).v
class K
  def self.new(x) = super(x + 50)
end
p k.new(2).v
