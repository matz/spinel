class K
  attr_reader :v
  def initialize(x) = @v = x
end
p K.new(1).v
class K
  def self.new(x) = super(x * 7)
end
p K.new(2).v
l = lambda { class Z; def self.new(*a) = :z; end; Z }
p l.call.new
