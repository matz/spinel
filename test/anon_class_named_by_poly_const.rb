# A constant whose value has no fixed kind (`X = [Class.new].first`, a
# method's answer, a Hash's value) names the anonymous class or module it
# holds, as CRuby names any class a constant's first write holds. A class
# already named keeps its name; a constant holding something else is left
# alone; the name is the constant's under its namespace.
X = [Class.new].first
p X.name
p X
p X.new.class
M = [Module.new].first
p M.name

def pick(a) = a.last
Y = pick([1, Class.new])
p Y.name
Y2 = pick([1, Y])
p Y2.name, Y.name

H = { k: Class.new }
Z = H[:k]
p Z.name, H[:k].name

module Outer
  Inner = [Class.new].first
  p Inner.name
end
p Outer::Inner.name

N = [42].first
p N
S = ["str"].first
p S
