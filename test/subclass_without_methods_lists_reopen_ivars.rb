# A subclass of an Array, a Hash or a builtin exception that defines no
# method of its own (and a grandchild under one) holds the ivars its
# builtin's reopening keeps in the object-keyed map: reflection lists
# them and dup copies them.
class Array
  def tag!(v) = (@tag = v; self)
  def tag = @tag
end
class Plain < Array; end
class Base < Array
  def btag = @tag
end
class Leaf < Base; end
class Hash
  def mark!(v) = (@mark = v; self)
  def mark = @mark
end
class Bag < Hash; end
class StandardError
  def code!(v) = (@code = v; self)
  def code = @code
end
class AppError < StandardError; end

a = Plain.new.tag!(:p)
p a.instance_variables, a.instance_variable_get(:@tag), a.dup.tag
l = Leaf.new.tag!(:l)
p l.instance_variables, l.btag, l.dup.tag
b = Bag.new.mark!(:m)
p b.instance_variables, b.instance_variable_get(:@mark), b.dup.mark
e = AppError.new("e").code!(7)
p e.instance_variables, e.instance_variable_get(:@code), e.dup.code
