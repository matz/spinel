# A `...` forwarder no call names, of a name outside fwd_uncalled_rest that
# the runtime calls in a fixed shape (to_i through Integer(), to_path
# through File), compiles as it did before forwarding took a rest.
class I
  def initialize = @n = 2.0
  def to_i(...) = @n.to_i(...)
end

class D
  def initialize = @s = "/"
  def to_path(...) = @s.to_s(...)
end

p Integer(I.new)
p File.directory?(D.new)
