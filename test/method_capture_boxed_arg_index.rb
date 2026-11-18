# The boxed argument of method_capture_boxed_arg.rb, where the Method is
# invoked with `[]`: on a boxed receiver that may hold a Method, `[]` is the
# call `.call` is. The same site indexes a Hash, an Array and a String, and
# calls a lambda.
class Registry
  def initialize = @slots = {}
  def put(key, value)
    @slots[key] = value
    value
  end
  def get(key) = @slots[key]
end
def look(source, key) = source[key]
registry = Registry.new
registry.put(1, 2)
p registry.get(1)
sources = [registry.method(:get), {1 => "hash"}, [7, 8, 9], ->(key) { key }, "string"]
keys = [1, "one", 2.5, nil]
p look(sources[0], keys[0]), look(sources[0], keys[1]), look(sources[0], keys[3])
p look(sources[1], keys[0]), look(sources[2], keys[0]), look(sources[3], keys[2]), look(sources[4], keys[0])
p [registry.method(:put), nil][0][keys[1], keys[2]]
p registry.get("one"), registry.get(1)

module Policy
  def self.check(value)
    p value.is_a?(Float)
    value
  end
end
def adapter(options)
  callback = options[:loader]
  proc { |value| callback[value] }
end
Policy.check(0)
method_loader = adapter({loader: Policy.method(:check)})
proc_loader = adapter({loader: ->(value) { value }})
p method_loader.call(1.5)
p proc_loader.call("s")
