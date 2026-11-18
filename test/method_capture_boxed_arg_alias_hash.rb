# The boxed argument of method_capture_boxed_arg.rb, with the option Hash
# aliased, merged and copied before the adapter reads the Method out of it:
# no walk from the call's receiver finds the `method(:sym)` node, and the
# parameter the direct call typed Integer still takes the Float.
module Policy
  def self.check(value)
    p value.is_a?(Float)
    value
  end
end
def adapter(options)
  opts = options
  merged = {strict: true}.merge(opts)
  copy = merged.dup
  callback = copy[:loader]
  proc { |value| callback.call(value) }
end
Policy.check(0)
method_loader = adapter({loader: Policy.method(:check)})
proc_loader = adapter({loader: ->(value) { value }})
p method_loader.call(1.5)
p proc_loader.call("s")
p Policy.check(7) + 1
