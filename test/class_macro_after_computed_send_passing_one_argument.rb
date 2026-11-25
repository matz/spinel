# A send of a computed name that passes the method one plain argument (no
# block, splat or keywords) makes no method of a name the load record cannot
# read either: Ruby's forms that take one (attr_reader, include,
# remove_method, private, class_eval, ...) are no arm of Spinel's computed
# send, which raises NoMethodError for them. Such a send anywhere in the
# program no longer turns every call into one that may define, so a
# class-body macro after a class-method call is expanded.
module Macros
  def attribute(name) = define_method(name) { name.to_s }
end

class Handler
  def initialize(n) = @n = n
  def run(o, arg) = o.send(@n, arg)
  def run_public(o, arg) = o.public_send(@n, arg)
  def run_dunder(o, arg) = o.__send__(@n, arg)
end

class Todo
  extend Macros
  def self.declare(n) = (@s ||= []) << n
  declare :x
  attribute :title
  def on_error(e) = "handled #{e}"
  def self.declared = @s
end

p Todo.new.title, Todo.declared
p Handler.new(:on_error).run(Todo.new, "boom")
p Handler.new(:on_error).run_public(Todo.new, "bang")
p Handler.new(:on_error).run_dunder(Todo.new, "oops")
