# `include A, B` at the top level puts A nearest Object, as `include B`
# then `include A` would: a bare call of a method both define reaches A's.
module A
  def m = "A"
  def only_a = "only A"
end
module B
  def m = "B"
  def only_b = "only B"
end
include A, B
puts m
puts only_a
puts only_b
