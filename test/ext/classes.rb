# Entries that take and answer instances of the program's classes: the
# contract header names their C types, so it declares them itself, and a
# host that includes the header alone compiles. TodoList is a heap object
# the host holds a pointer to, Pt a value type an entry answers by value
# (its whole struct in the header), Oops an exception subclass without
# ivars (the runtime's sp_Exception).
class TodoList
  attr_accessor :count
  def initialize = @count = 0
end
class Pt
  attr_reader :x, :y
  def initialize(x, y) = (@x = x; @y = y)
end
class Oops < StandardError
end
module ExtClasses
  #: () -> TodoList
  def self.new_list = TodoList.new
  #: (TodoList) -> Integer
  def self.add(l) = (l.count += 1)
  #: (Integer) -> Pt
  def self.pt(n) = Pt.new(n, n * 2)
  #: () -> Oops
  def self.oops = Oops.new("bad")
end
