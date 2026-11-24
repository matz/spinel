# A send of a computed name that passes the method nothing (no argument
# after the name, no block) makes no method of a name the load record cannot
# read: every form that names a method takes the name as an argument. Such a
# send anywhere in the program no longer turns every call into one that may
# define, so a class-body macro after a class-method call is expanded.
module Macros
  def attribute(name) = define_method(name) { name.to_s }
end

class Runner
  def initialize(n) = @n = n
  def run(o) = o.send(@n)
  def run_public(o) = o.public_send(@n)
end

class Todo
  extend Macros
  def self.declare(n) = (@s ||= []) << n
  declare :x
  attribute :title
  def hi = 1
  def self.declared = @s
end

p Todo.new.title, Todo.declared
p Runner.new(:hi).run(Todo.new)
p Runner.new(:title).run_public(Todo.new)
