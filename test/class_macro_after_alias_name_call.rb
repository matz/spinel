# A call of a name an alias makes runs a method of its old name, when the
# source spells it and the program alone makes it: the alias copies one of
# those. When no method of the old name defines, a class-body macro after
# such a call, on a receiver the load record does not know, is expanded.
class Day
  def self.civil(y, m = 1) = new_day(y * 100 + m)
  def self.new_day(n) = allocate.tap { |d| d.set(n) }
  def set(n) = @n = n
  def n = @n
  class << self
    alias_method :make, :civil
  end
  alias num n
  alias_method :count_of, :num   # an alias of an alias
end

module Macros
  def column(name)
    note(name)
    define_method(name) { "#{name} #{@k}" }
  end
end

def build(k, y) = k.make(y, 2)
def peek(d) = d.count_of

class Todo
  extend Macros
  def self.note(n) = (@cols ||= []) << n
  def self.cols = @cols
  def initialize = @k = 1
  D = build(Day, 20)
  peek(D)
  column :title
end

p Todo.new.title
p Todo.cols
p peek(build(Day, 7))
