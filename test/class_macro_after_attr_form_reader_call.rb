# A call of a name only attr forms make (attr_reader, attr_writer,
# attr_accessor) runs a reader, which reads an instance variable, or a
# writer, which writes one: neither defines a method. Such a call, on a
# receiver the load record does not know, no longer counts as one that may
# define, so a class-body macro after it is expanded.
class Item
  attr_reader :name
  attr_accessor :qty
  def initialize(n) = (@name = n; @qty = 0)
end

module Macros
  def column(name)
    note(name)
    define_method(name) { "#{name} #{@n}" }
  end
end

def label(x) = x.name
def grow(x, n) = (x.qty = n)
def mark(x, k) = (x.kind = k)
IT = Item.new("it")

class Todo
  extend Macros
  class << self; attr_accessor :kind; end
  def self.note(n) = (@cols ||= []) << [n, kind]
  def self.cols = @cols
  def initialize = @n = 2
  label(Item.new("x"))
  grow(IT, 3)
  column :title
  mark(self, :late)   # a writer run on the class, which the next macro reads
  column :body
end

p Todo.new.title
p Todo.new.body
p Todo.cols
p label(IT), IT.qty
