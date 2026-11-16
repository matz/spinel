# A send of a computed setter name hands its one value to the writers of the
# receiver's classes: the other ivars they write keep their classes
class A
  def hi = "A"
end
class B
  def hi = "B"
end
class Config
  attr_accessor :column, :label
  def initialize
    @column = A.new
    @label = nil
  end
end
c = Config.new
key = ARGV[0] || "label"
c.send("#{key}=", B.new)
p c.label.hi
p c.column.hi
