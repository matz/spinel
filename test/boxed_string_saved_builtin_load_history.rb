class BoxedStringAliasPeer
  def size = 91
  def <=>(other) = nil
end

def show_readers(value)
  p [value.size, value <=> "z"]
end

text = +"abc"
value = [text, BoxedStringAliasPeer.new][0]
show_readers(value)
class String
  alias saved_size size
  alias_method :saved_size_again, :saved_size
  alias saved_comparison <=>
  def size = 8
  def <=>(other) = 42
end
show_readers(value)
class String
  alias size saved_size_again
  alias_method :<=>, :saved_comparison
end
show_readers(value)
method = text.method(:size)
p [method.name, method.original_name, method.call]
