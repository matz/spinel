# `method(:name)` on an instance of a builtin's subclass captures the
# builtin's own method where the subclass defines none of the name, as it does
# the subclass's own.
class Inq < String
  def shout = upcase + "!"
end
class Bag < Array
  def second = self[1]
end
e = Inq.new(ARGV[0] || "development")
m = e.method(:upcase)
p m.call, m.owner, m.arity, m.name
p e.method(:shout).call, e.method(:length).call, e.method(:start_with?).call("dev")
b = Bag.new([3, 1, 2])
p b.method(:size).call, b.method(:second).call, b.method(:sort).call, b.method(:include?).call(2)
