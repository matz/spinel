# `lv OP= rhs` on a boxed local that may hold a Vec reaches Vec#+ through
# the runtime's operator dispatch, which hands the argument over boxed: the
# parameter takes the box. (opassign_poly_slot_class_facts.rb is a local
# that holds no Vec.)
Vec = Struct.new(:x, :y) do
  def +(other) = Vec.new(x + other.x, y + other.y)
end
mixed = [3, Vec.new(1.0, 2.0)]
acc = mixed[1]
acc += Vec.new(0.25, 0.25)
p acc.x, acc.y
acc = mixed[0]
acc += 4
p acc
