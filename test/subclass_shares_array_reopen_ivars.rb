# A program subclass of an Array or a Hash shares, with a reopening of the
# builtin, the ivars that reopening keeps in the object-keyed map: either
# side sees the other's writes, also through attr_reader, a reopening of the
# subclass with no superclass written, a grandchild, defined? and ||=, and
# reflection and dup see them. Held as the subclass's fields, the two
# stores never met.
class Array
  def tag!(v)
    @tag = v
    self
  end
  def tag = @tag
end
class Stack < Array
  attr_reader :tag
  def initialize(*a)
    super
    @own = :mine
  end
  def info = [@own, @tag, defined?(@tag)]
  def tag_or!(v)
    @tag ||= v
    self
  end
end
class Stack
  def again = @tag
end
class Deep < Stack
  def deep = [@tag, @own]
end
s = Stack.new
p s.info
s.tag_or!(:first).tag_or!(:second)
p s.tag, s.again, s.info
p s.instance_variables
d = Deep.new.tag!(:deep)
p d.deep, d.tag, d.again
p d.instance_variables.sort
c = s.dup
p c.tag
class Hash
  def mark!(v)
    @mark = v
    self
  end
  def mark = @mark
end
class Bag < Hash
  def sub_mark = @mark
  def sub_mark!(v)
    @mark = v
    self
  end
end
b1 = Bag.new.mark!(:from_hash)
p [b1.mark, b1.sub_mark]
b2 = Bag.new.sub_mark!(:from_bag)
p [b2.mark, b2.sub_mark]
p b1.instance_variables, b1.instance_variable_get(:@mark)
