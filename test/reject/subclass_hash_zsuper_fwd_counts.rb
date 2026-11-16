# A bare `super` from a `(...)` method into a builtin method: the forwarder's
# parameters are the most any call passes, and Hash#fetch takes what each call
# passes, so calls with different counts cannot be spelled yet: refused.
# spinel: reject-subclass: `...` forwarded into a builtin method, from calls that do not all pass the same number
module Wrap
  def fetch(...)
    super
  end
end

class Registry < Hash
  include Wrap
end

h = Registry.new
h[:a] = 1
p h.fetch(:a)
p h.fetch(:z, 9)
