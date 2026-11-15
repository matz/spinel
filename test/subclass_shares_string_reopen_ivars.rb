# A program subclass of String shares, with a reopening of String, the
# ivars that reopening keeps in the object-keyed map, as an Array or a Hash
# subclass does: either side sees the other's writes, also through
# attr_reader, and reflection and dup see them.
class String
  def tag!(v)
    @tag = v
    self
  end
  def tag = @tag
end
class Text < String
  attr_reader :tag
  def initialize(s)
    super
    @own = 1
  end
  def info = [@own, @tag]
  def sub_write!(v)
    @tag = v
    self
  end
end
x = Text.new("x")
x.tag!(:p)
p x.info, x.tag, x.instance_variables
y = x.dup
p y.tag, y.info
z = Text.new("z").sub_write!(:from_sub)
p z.tag, z.info, z.instance_variable_get(:@tag)
