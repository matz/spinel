# A subclass of Array (Hash) with its own sum/max sends its instances to
# its own method; a plain one keeps the builtin, also when the reopened
# class's def calls super.
class Object
  def sum = 99
  def max = 98
end
class Array
  def sum = super
end
class MyA < Array
  def sum = 7
end
class Hash
  def max = super
end
class MyH < Hash
  def max = 6
end
def pick(i, a, b) = i == 0 ? a : b
p pick(0, [5, 4], 1).sum
p pick(0, MyA.new([1]), 1).sum
p pick(0, {a: 1, b: 2}, 1).max
p pick(0, MyH.new, 1).max
[[1, 2], MyA.new([3])].each { |v| p v.sum }
