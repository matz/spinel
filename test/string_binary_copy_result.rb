class Label < String
  def copy
    b
  end
end

class Forwarding < String
  def b
    super
  end
end

class Override < String
  def b
    :override
  end
end

def boxed(value, index)
  [value, 12][index]
end

source = +"é\0z"
first = source.b
second = boxed(source, 0).b
p [first.bytes, first.encoding.name, first.frozen?, first.equal?(source), first.equal?(second)]
p [second.encoding.name, second.frozen?, second.equal?(source)]
first << "!"
p [first.bytes, second.bytes, source.bytes]
p ["".b.bytesize, "".b.encoding.name, "".b.equal?("".b)]
p [Label.new("é").copy.class.name, Label.new("é").b.encoding.name, Forwarding.new("é").b.bytes]
p [Override.new("").b, boxed(Override.new(""), 0).b]
begin
  boxed(source, 1).b
rescue NoMethodError => error
  p error.name
end

def nullable_copy(flag)
  value = flag ? +"é" : nil
  value&.b&.bytes
end

p nullable_copy(false)
p nullable_copy(true)
