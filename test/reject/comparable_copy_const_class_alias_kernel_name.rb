# An alias of p in `class Other`, where the constant Other holds String
# by a computed value: CRuby reopens String, so the Comparable method
# prints O1. Spinel's class table opens a class of its own there, so an
# alias in a class whose constant the program writes counts. Refused.
Other = String.itself
class Other
  def original(x) = "O#{x}"
  alias p original
end
module Comparable
  def qa = p(1)
end
puts "x".qa
