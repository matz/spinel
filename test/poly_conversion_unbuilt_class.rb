# A class nothing constructs can own the name a boxed value's builtin answers (`to_str`):
# the dispatch builds no arm for it, so the call answers the builtin's String.
class Unbuilt
  def to_str
  end
end

class UnbuiltText
  def to_str
    "unbuilt"
  end
end

def convert(value)
  converted = value.to_str
  converted
end

x = ARGV.size > 5 ? 1 : "s"
p convert(x)
y = ARGV.size > 5 ? 1 : "t"
converted = y.to_str
p converted
