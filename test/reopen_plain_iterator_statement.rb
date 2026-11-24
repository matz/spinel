# A reopened Array or Hash method that does not yield, called with a block
# as a statement, runs the program's method, as CRuby does: the builtin's
# loop of the same name does not answer for it.
class Array
  def each
    puts "mine"
    self
  end
end
b = [1, 2]
b.each { |e| p e }
a = [3, "x"]
a.each { |e| p e }
@c = [1, 2]
@c.each { |e| p e }
p(b.each { |e| p e })
h = { a: 1 }
class Hash
  def each = puts("hash mine")
end
h.each { |k, v| p k }
