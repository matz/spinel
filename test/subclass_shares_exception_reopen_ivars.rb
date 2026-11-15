# A program subclass of a builtin exception shares, with a reopening of
# the builtin exception, the ivars that reopening keeps in the
# object-keyed map: across a raise and a rescue, through attr_reader, a
# reopening of the subclass, a grandchild, reflection and dup.
class StandardError
  def tag!(v)
    @tag = v
    self
  end
  def tag = @tag
end
class AppError < StandardError
  attr_reader :tag
  def initialize(m = nil)
    super
    @code = 7
  end
  def info = [@code, @tag]
end
class AppError
  def again = @tag
end
class DeepError < AppError
  def deep = [@tag, @code]
end
begin
  raise AppError.new("a").tag!(:raised)
rescue AppError => e
  p e.tag, e.again, e.info, e.message
end
d = DeepError.new("d").tag!(:deep)
p d.deep, d.tag, d.instance_variables.sort
c = d.dup
p c.tag, c.deep
p RuntimeError.new("r").tag!(:plain).tag
