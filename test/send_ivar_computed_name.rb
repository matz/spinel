# public_send and send of a computed Symbol that a caller passes from an
# instance variable to a method's parameter
class Controller
  def initialize(name)
    @name = name
  end

  def run = dispatch(@name)
  def dispatch(name) = public_send(name)
  def late = 1
end

p Controller.new("late!".chomp("!").to_sym).run

# an ivar written from a literal Symbol in one place and a computed Symbol
# in another
class Mixed
  def initialize = @step = :first
  def first = :one
  def second = :two
  def advance(s) = @step = s.to_sym
  def run = call_step(@step)
  def call_step(s) = send(s)
end

m = Mixed.new
p m.run
m.advance("second")
p m.run

# a receiver of more than one class
class A
  def ping = :a
end

class B
  def ping = :b
end

class Router
  def initialize(name) = @name = name
  def to(o) = forward(o, @name)
  def forward(o, name) = o.public_send(name)
end

r = Router.new("pingx".chomp("x").to_sym)
p r.to(A.new)
p r.to(B.new)
p [1, "s"].map { |v| Router.new("to_s".to_sym).to(v) }

# an ivar of literal names only
class Plain
  def initialize = @n = :hello
  def run = say(@n)
  def say(n) = send(n)
  def hello = "hi"
end

p Plain.new.run
