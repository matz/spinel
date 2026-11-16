# A typed Array's index answer that is nil calls nil's own `itself` and
# `to_s`, where the program redefines them on Integer; a found one calls the
# program's.
class Integer
  def itself = 7
  def to_s = "int"
end
x = [1, 2].index(ARGV.size == 0 ? 9 : 2)
p x.itself, x.to_s
y = [1, 2].index(2)
p y.itself, y.to_s
