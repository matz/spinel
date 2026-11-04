# Protocol methods from reopenings retain their per-class answers. In
# particular TrueClass may define a name while FalseClass does not, despite
# their sharing one boxed tag.
class Float
  def reflection_protocol = :float
end
class Symbol
  def reflection_protocol = :symbol
end
class NilClass
  def reflection_protocol = :nil
end
class TrueClass
  def reflection_protocol = :true
end
class String
  def reflection_protocol = :string
end

def probe(value)
  p value.respond_to?(:reflection_protocol)
  p value.respond_to?("reflection_protocol")
  p value.reflection_protocol if value.respond_to?(:reflection_protocol)
  p value.respond_to?(:absent_protocol)
end
[7, 1.5, :sym, nil, true, false, "s", []].each { |value| probe(value) }
