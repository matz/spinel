# A method a direct call typed, captured with method(:sym) and read back out
# of an option Hash: the adapter calls whatever callable the Hash holds with
# a boxed value, whose kind only the run time knows. The direct call
# `Policy.check(0)` typed the parameter Integer and saw only its own
# argument, so the Float in the boxed value was refused at the Method's gate
# (`wrong argument type Float (expected Integer)`) where CRuby runs the body
# with it. A parameter a direct call typed takes the boxed argument a dynamic
# Method call hands its position; the boxed bound-Method ABI carries it
# (#4671).
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
method_loader = adapter({loader: Policy.method(:check)})
proc_loader = adapter({loader: ->(value) { value }})
p method_loader.call(1.5)
p proc_loader.call("s")
