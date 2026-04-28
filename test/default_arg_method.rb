# Issue #49: instance method with a default argument should compile
# both `g.hello("Ruby")` and `g.hello` — the no-arg call site needs
# the default value substituted in.

class Greeter
  def hello(name = "world")
    puts "Hello, #{name}"
  end
end

g = Greeter.new
g.hello              # Hello, world
g.hello("Ruby")      # Hello, Ruby
