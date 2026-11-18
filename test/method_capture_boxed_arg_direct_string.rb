# The boxed argument of method_capture_boxed_arg.rb, with a direct call
# passing a String after the Float went through the container, and a nil
# through it after that.
module Policy
  def self.check(value)
    p value.is_a?(Float)
    value
  end
end
def adapter(options)
  callback = options[:loader]
  proc { |value| callback.call(value) }
end
Policy.check(0)
loader = adapter({loader: Policy.method(:check)})
other = adapter({loader: ->(value) { value }})
p loader.call(1.5)
p Policy.check("s")
p loader.call(nil)
p other.call(:sym)
p Policy.check(2) + 1
