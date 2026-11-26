# String#squeeze takes its character sets from a single splat, as delete
# and count do: none (every run), one set, or several (their intersection)
a = ["x"]
p "xxbxx".squeeze(*a)

class Sets
  def initialize(sets) = @sets = sets
  def squeeze(str) = str.squeeze(*@sets)
end
p Sets.new(["x"]).squeeze("xxbxx"), Sets.new([]).squeeze("xxbx  "), Sets.new(["x", "bx"]).squeeze("xxbbxx")

def squeezed(*sets) = "xxbxx  ".squeeze(*sets)
p squeezed("x"), squeezed, squeezed("x", " ")

none = []
p "xxbx  ".squeeze(*none)
