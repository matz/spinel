# A String ivar mutated through its reader: the mutation has to reach the
# ivar, and an alias taken from the reader beforehand is the same object.

class C1
  attr_reader :name
  def initialize = @name = +"ab"
end
def alias_then_append
  c = C1.new
  x = c.name
  c.name << "!"
  p x
  p c.name
end
alias_then_append

class C2
  attr_reader :name
  def initialize = @name = +"ab"
end
def alias_then_other_mutators
  c = C2.new
  x = c.name
  c.name[0] = "X"
  p x, c.name
  d = C2.new
  y = d.name
  d.name.upcase!
  p y, d.name
  e = C2.new
  z = e.name
  z << "?"
  p e.name
  f = C2.new
  w = f.name
  f.name.insert(1, "-")
  p w, w.equal?(f.name)
end
alias_then_other_mutators
