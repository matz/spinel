# A store on an element of an Array a method returns reaches the element's
# method_missing, where a send with a run-time name in method_missing lets
# the element hold a value of any class (Rack::Files' `response[1][key] =
# value` over a body proxy). The assignment evaluates to the value it
# assigns, not to method_missing's :from_mm, and a store the wrapped Hash
# has no method for raises NoMethodError.
class Proxy
  def initialize(b) = @b = b
  def method_missing(name, *args, &blk)
    @b.__send__(name, *args, &blk)
    :from_mm
  end
  def respond_to_missing?(*) = true
end

def triple(status, h) = [status, {}, Proxy.new(h)]

def serve(size, h)
  response = triple(416, h)
  stored = (response[2]["k"] = size * 2)
  other = (response[2].x = 5) rescue $!.class
  [stored, other]
end

h = {}
p serve(3, h), h
