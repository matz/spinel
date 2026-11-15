# The Jar's Hash keeps its String keys and values although the Array its
# lines come from is filled by a block whose value is not typed yet on the
# first round (infer-test checks the layout).
class Jar
  def initialize
    @values = { "" => "" }
    @values.clear
  end

  def parse(line)
    name = line.byteslice(0, line.index("=") || line.bytesize)
    @values[name] = line unless @values.key?(name)
    self
  end

  def [](name) = @values.fetch(name, "")
  def size = @values.size
end

class Request
  def initialize
    @headers = [""]
    @headers.clear
    each_header { |name, value| @headers << name << value }
  end

  def each_header
    yield "cookie", "a=1"
    yield "cookie", "b=2"
    yield "host", "example.com"
  end

  def cookies
    jar = Jar.new
    i = 0
    while i < @headers.size
      jar.parse(@headers[i + 1]) if @headers[i] == "cookie"
      i += 2
    end
    jar
  end
end

jar = Request.new.cookies
puts jar["a"]
puts jar["b"]
puts jar["c"].empty?
puts jar.size
