# Exception's `exception` hands its keywords to initialize as a positional Hash,
# whatever else initialize takes. What that Hash holds reaches what the instance
# answers, as it does through a Hash written out.
class Foo
  def hello = "foo"
end

class Bar
  def hello = "bar"
end

class HashAndKeyword < StandardError
  attr_reader :v, :y

  def initialize(h, y: 0)
    super("m")
    @v = h[:x]
    @y = y
  end

  def go = @v.hello

  def self.make = exception(x: Foo.new)
end

class Inherits < HashAndKeyword; end

class LeadHashAndKeyword < StandardError
  attr_reader :a, :v

  def initialize(a, h, y: 0)
    super("m")
    @a = a
    @v = h[:x]
  end
end

class DefaultHashAndKeyword < StandardError
  attr_reader :v

  def initialize(h = {}, y: 0)
    super("m")
    @v = h[:x]
  end
end

class RestAndKeyword < StandardError
  attr_reader :v

  def initialize(*a, y: 0)
    super("m")
    @v = a.first[:x]
  end
end

class HashOnly < StandardError
  attr_reader :v

  def initialize(h)
    super("m")
    @v = h[:x]
  end
end

class KeywordOnly < StandardError
  def initialize(x: nil)
    super("m")
  end
end

puts HashAndKeyword.exception(x: Foo.new).v.hello
puts HashAndKeyword.exception(x: Foo.new).go
puts HashAndKeyword.exception(x: Foo.new).y
puts HashAndKeyword.make.v.hello
puts Inherits.exception(x: Bar.new).v.hello
puts LeadHashAndKeyword.exception(1, x: Foo.new).v.hello
puts LeadHashAndKeyword.exception(2, x: Bar.new).a
puts DefaultHashAndKeyword.exception(x: Foo.new).v.hello
puts RestAndKeyword.exception(x: Foo.new).v.hello
puts HashOnly.exception(x: Foo.new).v.hello

# a double splat, alone and beside a key
o = { x: Bar.new }
puts HashAndKeyword.exception(**o).v.hello
puts HashAndKeyword.exception(**o, z: 1).go

# two values through the one Hash
puts HashAndKeyword.exception(x: 3).v
puts HashAndKeyword.exception(x: "s").v

# a Hash written out reads the same, and its keyword stays a keyword at new
puts HashAndKeyword.exception({ x: Foo.new }).go
puts HashAndKeyword.new({ x: Foo.new }, y: 4).y
puts HashAndKeyword.new({ x: Bar.new }, y: 5).go

# by send
puts HashAndKeyword.send(:exception, x: Foo.new).go
puts HashAndKeyword.public_send(:exception, x: Bar.new).v.hello

# on a class value, out of an Array, a parameter and a Hash
k = [HashAndKeyword].first
puts k.exception(x: Foo.new).v.hello
puts k.exception(x: Bar.new).go

def through(cls) = cls.exception(x: Foo.new)
puts through(HashAndKeyword).v.hello
puts through(Inherits).go
puts({ c: Inherits }[:c].exception(x: Bar.new).go)

# a class whose initialize takes keywords alone has no parameter for the Hash
begin
  KeywordOnly.exception(x: 1)
rescue ArgumentError => e
  puts e.class
end
