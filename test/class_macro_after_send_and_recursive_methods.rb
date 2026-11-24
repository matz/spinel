# A class-body macro call after a class method call is expanded when what
# that call may do is known to define nothing. The load record answers it
# by the program's methods of each name the call may run, so a method that
# reaches a method of its own name again (a cycle), or a send of a name the
# source spells, made it answer "may define" for every later macro, which
# was then refused ("Module#define_method with a non-literal name").
module Macros
  def attribute(name) = define_method(name) { name.to_s }
end

# a method that calls a method of its own name
class Day
  def initialize(n) = @n = n
  attr_reader :n
  def +(k) = k > 1 ? (self + 1) + (k - 1) : Day.new(@n + k)
end

# __send__ of a literal name, which calls back into a cycle
class Week
  def initialize(n) = @n = n
  def -(k) = __send__(:minus, k)
  def minus(k) = Week.new(@n - k)
  def count = @n
end

class Todo
  extend Macros
  def self.declare(n) = (@s ||= []) << n
  def self.declared = @s
  declare :x
  attribute :title
  declare :y
  attribute :body
end
p Todo.new.title, Todo.new.body, Todo.declared
p (Day.new(1) + 3).n
p (Week.new(9) - 4).count
