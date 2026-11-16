# A class whose superclass is a builtin (an exception, an Array) reaches the
# builtin's method with super before a reopening of Object's method of the
# name: Object comes after the builtin in the ancestors. A name the builtin
# does not answer goes on to the reopening.
class Object
  def to_s = "obj"
  def last = :obj
  def tap_name = :obj_tap
end
class E < StandardError
  def to_s = "E:" + super
end
p E.new("m").to_s
class A < Array
  def last = super
  def tap_name = super
end
p A.new([1, 2]).last
p A.new([1, 2]).tap_name
