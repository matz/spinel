# Three reopened builtin exceptions in a line (StandardError, RuntimeError, and
# FrozenError below RuntimeError): each initialize's super reaches the next
# reopening's initialize up the builtin chain, and the last one the builtin's.
class StandardError
  def initialize(msg = nil) = super("S[#{msg}]")
end
class RuntimeError
  def initialize(msg = nil) = super(msg.to_s + "!")
end
class FrozenError
  def initialize(msg = nil, receiver: nil) = super("F:" + msg.to_s)
end
p RuntimeError.new("r").message
p FrozenError.new("f").message
begin
  raise RuntimeError, "boom"
rescue RuntimeError => e
  p e.message
end
