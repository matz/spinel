# spinel: reject-unsupported: super in `self.new` of a Struct, Data, exception or native class
# The same with an explicit super(*a), in a reopened RuntimeError.
class RuntimeError
  def self.new(*a) = super(*a)
end
p RuntimeError.new("x").message
