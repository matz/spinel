# A bare `print KEYS` in a class body calls the class's own print, which
# can change the list (here it adds :c), so Struct.new(*KEYS) reads names
# known only at run time and is refused. Spinel built a Struct of [:a, :b].
KEYS = [:a, :b]
class Writer
  def self.print(keys) = keys << :c
  print KEYS
end
R = Struct.new(*KEYS)
p R.members
