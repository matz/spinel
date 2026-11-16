class Base
  attr_reader :v
  def initialize(x) = @v = x
end
class Kid < Base; end
p Kid.new(1).v
class Base
  def self.new(x) = super(x * 7)
end
p Kid.new(2).v
p Base.new(3).v
