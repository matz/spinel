# send with a runtime name, from a base class's method, of an attr_reader
# only a subclass declares
class Drop
  def invoke(key) = send(key.to_s)
  def to_s = "drop"
end
class Loop < Drop
  def initialize(p) = @parentloop = p
  attr_reader :parentloop
  def index = 2
end
class Row < Drop
  attr_reader :row
  def initialize = @row = 7
end
l = Loop.new(:outer)
p l.invoke(:index)
p l.invoke(:parentloop)
p Row.new.invoke(:row)
p Row.new.invoke("to_s")
