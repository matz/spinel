# A `...` forwarder of `==` no call names, reached only through Array's
# searches, takes the argument they pass.
class V
  def initialize(v) = @v = v
  def ==(...) = @v.==(...)
end

p [V.new(1)].include?(1), [V.new(1)].index(1), [V.new(1)].count(1)
