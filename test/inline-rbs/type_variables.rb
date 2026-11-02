# Inline signatures in reopenings of builtin generics use the type
# variables core RBS declares for them (`class Array[unchecked out Elem]`,
# `Hash[K, V]`), as Active Support writes `Array#from` and `Array#second`:
#
#   #: (Integer) -> Array[Elem]
#   #: () -> Elem?
#
# Such a type is the receiver's element type, which differs from receiver
# to receiver: the slot pins nothing, as `top` does, and the rest of the
# signature applies. The `untyped` slots beside them pin what inference
# would not choose, so the C shows the signature applied, and the program
# compiled with `--no-inline-rbs --rbs sig/type_variables` (which declares
# the variables) produces the same C byte for byte.
class Array
  #: (Integer, untyped) -> Array[Elem]
  def from_pos(position, x) = self[position, length] || []

  #: (untyped) -> Elem?
  def second_one(x) = self[1]

  #: (Elem, untyped) -> bool
  def has?(e, x) = include?(e)
end

class Hash
  #: (K, untyped) -> V?
  def at_key(k, x) = self[k]
end

p [1, 2, 3].from_pos(1, nil), %w[a b].from_pos(5, 1), [1, 2].second_one(:x), ["a"].second_one(2)
p [1, 2].has?(2, nil), %w[a].has?("b", 1)
p({ a: 1 }.at_key(:a, nil), { "x" => "y" }.at_key("z", 1))
