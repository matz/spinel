# The captured builtin compares with its original arity and source location.
module ComparisonLayer
  def <=>(other) = 7
end

class ComparisonPeer
  def <=>(other) = 81
end

def public_comparison(value)
  value.public_method(:<=>)
end

text = +"abc"
mixed = [text, ComparisonPeer.new][0]
class String
  prepend ComparisonLayer if ENV["SPINEL_TEST_METHOD_PREPEND"] == "1"
end

typed = text.method(:<=>)
boxed = public_comparison(mixed)
class String
  def <=>(other) = 19
end

p [typed.arity, boxed.arity]
p typed.source_location.nil?
p [typed.call("abc"), boxed.call("abd"), typed.call(nil)]
p text.method(:<=>).call("abc")
