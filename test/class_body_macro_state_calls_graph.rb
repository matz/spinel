# A class-body macro (a module's method the class extends, run by the walk
# at compile time) that calls the class's own methods writing its state,
# and a class-body call of such a method ahead of the macro, in a program
# whose macros define methods by computed names. Under the graph load model
# each was refused ("define_method with a non-literal name"):
#   - a nested macro call whose frame defines nothing, only leaves
#     statements as written (`declare_attribute(name)`), could not be
#     placed: the call itself is now left as written, its arguments bound;
#   - a statement that reads a self call's value (`has_manys << name`);
#   - a class-body call ahead of the macro (`reg :a`) was taken for code
#     that may define methods, because a computed define_method may define
#     any name: such a method does what its block does, here nothing, and
#     code that only writes instance variables no longer bars the macro.
module Macros
  def attribute(name, type = :string)
    declare_attribute(name.to_s, type)
    define_method(name) { "#{name}:#{type}" }
  end
  def has_many(name)
    has_manys << name
    keep(name)
    define_method(name) { [name] }
  end
  def counted(name)
    note(name)
    define_method(name) { self.class.seen.size }
  end
end

class Model
  extend Macros
  def self.schema = (@schema ||= {})
  def self.declare_attribute(n, t) = schema[n] = t
  def self.has_manys = (@has_manys ||= [])
  def self.keep(x) = (@kept ||= []) << x
  def self.kept = @kept
  def self.note(n) = (@seen ||= []) << n
  def self.seen = @seen
end

class Todo < Model
  reg = :ignored
  note :pre
  attribute :title
  attribute :done, :bool
  has_many :tags
  counted :a
  @seen << :mid
  counted :b
end

t = Todo.new
p t.title, t.done, t.tags, t.a, t.b
p Todo.schema, Todo.has_manys, Todo.kept, Todo.seen

class Note < Model
  attribute :body
end
p Note.new.body, Note.schema, Todo.schema
