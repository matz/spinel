# A method named `a` is no method the runtime calls back by itself. The
# callbacks table had taken the `a` that `inject("a")` sends to an element,
# the name its probe was told to call, for one, so a class's `a` of no
# effect, defined after a class whose macro follows a class-method call,
# answered "may define", and that macro was refused.
module Macros
  def attribute(name) = define_method(name) { name.to_s }
end

class Todo
  extend Macros
  def self.declare(n) = (@s ||= []) << n
  declare :x
  attribute :title
end

class Note
  extend Macros
  def self.a(n) = nil
  def self.b(n) = n > 0 ? a(n - 1) : c
  def self.c = nil
  attribute :title
end

class Pair
  def initialize(v) = @v = v
  attr_reader :v
  def a(o) = Pair.new(v + o.v)
end

p Todo.new.title, Note.new.title
p Note.b(2)
# inject and reduce with no block still call the method they are given
p [Pair.new(1), Pair.new(2), Pair.new(3)].inject(:a).v
p [Pair.new(4), Pair.new(5)].reduce(Pair.new(1), :a).v
