# A module prepended to Range whose step takes any arguments wins over
# Range#step: a step written without an argument reaches it with none, and a
# step given one with that one.
module M
  def step(*a) = [:m, a]
end
class Range
  prepend M
end
p (1..6).step
p (1..6).step(2)
p (1...6).step
