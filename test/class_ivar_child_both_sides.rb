# a subclass's class-level @x beside the instance @x its parent writes:
# each side keeps its own slot
class Pa1
  def initialize = (@x = 1)
  def x = @x
end
class Ch1 < Pa1
  @x = 7
  def self.cx = @x
  def y = @x
end
p Ch1.cx
p Ch1.new.y
p Ch1.new.instance_variables

# a class method's read of an @x only the parent's instances write: the
# class's own @x, unset
class Pa2
  def initialize = (@x = "pa")
  def x = @x
end
class Ch2 < Pa2
  def self.cx = @x
  def y = @x
end
p Ch2.cx
p Ch2.new.y
