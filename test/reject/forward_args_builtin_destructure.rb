# A `...` forwarder into a builtin of its own name, with a destructuring
# leading parameter, is refused: the destructured `(x, y)` is not passed
# to the builtin as it is.
class Box
  def first((x, y), ...) = [1, 2, 3].first(...)
end
p Box.new.first([0, 0], 2)
