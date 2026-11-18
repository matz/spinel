# A prepend's super uses the builtin arity until the own definition runs.
module ReadingDelegate
  def size = super
  def <=>(other) = super(other)
end

class OtherPrependReader
  def size = 90
  def <=>(_other) = 91
end

def size_at(value)
  value.size
rescue ArgumentError
  :arity
end

def compare_at(value)
  value <=> "zzz"
rescue ArgumentError
  :arity
end

value = [+"abc", OtherPrependReader.new][0]
p [size_at(value), compare_at(value)]
class String
  prepend ReadingDelegate
end
p [size_at(value), compare_at(value)]
class String
  def size(value) = value + 10
  def <=>(_left, _right) = 12
end
p [size_at(value), compare_at(value)]
