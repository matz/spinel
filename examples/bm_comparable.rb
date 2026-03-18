# Test alias

class Greeter
  def initialize(name)
    @name = name
  end

  def name
    @name
  end

  def hello
    @name
  end

  alias greet hello
end

g = Greeter.new("world")
puts g.hello  # world
puts g.greet  # world (alias of hello)
puts g.name   # world

puts "done"
