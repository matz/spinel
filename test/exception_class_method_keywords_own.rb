# Keywords reach a class method `exception` of the program's as keywords, while
# a class of Exception beside it hands them to initialize as a positional Hash.
class Foo
  def hello = "foo"
end

class Bar
  def hello = "bar"
end

class OwnException
  attr_reader :y

  def initialize(y)
    @y = y
  end

  def self.exception(y: 0) = new(y)
end

class HashAndKeyword < StandardError
  attr_reader :v, :y

  def initialize(h, y: 0)
    super("m")
    @v = h[:x]
    @y = y
  end
end

puts OwnException.exception(y: Foo.new).y.hello
puts OwnException.exception.y
puts HashAndKeyword.exception(x: Foo.new).v.hello
puts HashAndKeyword.exception(x: Bar.new).y
puts HashAndKeyword.new({ x: Foo.new }, y: 6).y
