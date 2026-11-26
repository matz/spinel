# a store into an element that may be a method_missing class, as rack's
# files.rb writes `response[1]["content-range"] = ...`: the rewrite splits
# the call and holds the value once, and a later run of the pass does not
# hold it again (make infer-test reads the C for a nested hold)
class Proxy
  def initialize(b) = @b = b
  def method_missing(name, *args, &blk) = @b.__send__(name, *args, &blk)
  def respond_to_missing?(*) = true
end

def fail(status, msg) = [status, { "content-type" => "text/plain" }, Proxy.new([msg])]

def serve(size)
  response = fail(416, "unsatisfiable")
  v = (response[1]["content-range"] = "bytes */#{size}")
  [response, v]
end

r, v = serve(3)
p r[0], r[1], v
