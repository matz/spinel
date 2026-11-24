# A `...` forwarder of `to_str` no call names, reached only through a
# String's implicit conversion, takes no argument.
class V
  def initialize = @s = "z"
  def to_str(...) = @s.to_str(...)
end

p "a" + V.new
