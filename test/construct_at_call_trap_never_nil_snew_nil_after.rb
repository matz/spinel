class K
  def initialize = @x = 1
end
a = K.new
p a.nil?
class K
  def self.new = nil
end
b = K.new
p b.nil?
p b
