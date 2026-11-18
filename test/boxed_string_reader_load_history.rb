# A boxed call in the same method sees a String builtin and its replacement.
class OtherReader
  def length = 10
  def size = 11
  def empty? = true
end

module StringReaderPrepend
  def empty? = true
end

def readers(value)
  [value.length, value.size, value.empty?, value <=> "zzz"]
end

value = [+"abc", OtherReader.new][0]
p readers(value)

class String
  alias original_size size
  def length = 21
  def shifted_size = 22
  alias size shifted_size
  define_method(:<=>) { |_other| 23 }
  prepend StringReaderPrepend
end

p readers(value)
p (+"abc").original_size
