# A boxed value that may be a Class, asked a name an instance method and a
# class method of the program answer too: the dispatch's class arm for any
# other class is Module#name, the class's name as bytes (nil for an
# anonymous class). The probe for that arm asked the call's own type, poly
# (a class method of the name that only raises answers nothing), and the
# arm handed the bytes to the boxed slot: the C did not build. The bytes
# are boxed as the String they are; to_s and inspect, the other readers of
# a class's name, the same.
class Node
  def initialize(n) = @n = n
  def name = @n
  def to_s = "node #{@n}"
  def inspect = "#<#{@n}>"
end
class Tasks
  def self.name = raise(NotImplementedError, "pruned")
  def self.to_s = raise(NotImplementedError, "pruned")
  def self.inspect = raise(NotImplementedError, "pruned")
end
def answer(t)
  yield
rescue NotImplementedError, NoMethodError => e
  "#{e.class}: #{e.message[0, 24]}"
end
anon = Class.new
things = [Node.new("x"), Node, Tasks, String, anon, :sym, "str"]
things.each { |t| p answer(t) { t.name } }
named = things - [anon]
puts named.map { |t| answer(t) { t.to_s } }.inspect
puts named.map { |t| answer(t) { t.inspect } }.inspect
