# a store into a value a method_missing class may answer, as rack's files.rb
# and static.rb write it: the value the assignment answers is held once, in
# the method's own scope, however often the pass meets the call
class Proxy
  def initialize(b) = @b = b
  def method_missing(name, *args) = name == :[]= ? @b.store(args[0], args[1]) : nil
  def respond_to_missing?(*) = true
end

CONTENT_TYPE = "content-type"

class Files
  def fail(status, body)
    [status, { CONTENT_TYPE => "text/plain", "content-length" => body.size.to_s }, [body]]
  end

  def serving(size)
    response = fail(416, "Byte range unsatisfiable")
    response[1]["content-range"] = "bytes */#{size}"
    response
  end
end

def static(r)
  r[1][CONTENT_TYPE] = r[0].to_s
  r
end

def store_both(response, size)
  v = (response[1]["content-range"] = "bytes */#{size}")
  response[1]["content-type"] = size.to_s
  [v, response]
end

p Files.new.serving(3)
p static([200, {}, ["x"]])[1]
p store_both([416, {}, []], 3)
p store_both([416, Proxy.new({}), []], 4)[1][1].class
