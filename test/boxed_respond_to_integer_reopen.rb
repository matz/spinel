# A protocol added to Integer is not part of its builtin surface. A boxed
# Integer must still answer through the reopening, not the absent-name guard.
class Integer
  def to_ary
    [self]
  end
end

def probe(value)
  p value.respond_to?(:to_ary)
  p value.respond_to?("to_ary")
  p value.to_ary if value.respond_to?(:to_ary)
end
[7, [], "s"].each { |value| probe(value) }
p 7.respond_to?(:to_ary)
