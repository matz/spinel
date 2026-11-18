# The boxed argument of method_capture_boxed_arg.rb, handed on by the
# captured method to a helper that stores it: the helper's parameter, the
# Array it lands in and the direct caller's `+ 1` follow the widened
# parameter.
class Sink
  def initialize = @seen = []
  def note(value)
    @seen << value
    value
  end
  def seen = @seen
end
SINK = Sink.new
def helper(value)
  SINK.note(value)
end
def check(value)
  kept = helper(value)
  kept
end
def adapter(options)
  callback = options[:loader]
  proc { |value| callback.call(value) }
end
p check(0)
loader = adapter({loader: method(:check)})
other = adapter({loader: ->(value) { value }})
p loader.call(1.5)
p other.call("s")
p loader.call("t")
p check(2) + 1
p SINK.seen
