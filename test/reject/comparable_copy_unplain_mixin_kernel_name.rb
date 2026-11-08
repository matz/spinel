# A module with its own p, mixed into Kernel by a call that Spinel's
# class table does not hold (`Kernel.prepend(M)`): CRuby prints M1 from
# the Comparable method. Spinel can not tell which classes M reaches, so
# the copy is refused.
module M
  def p(x) = "M#{x}"
end
Kernel.prepend(M)
module Comparable
  def qa = p(1)
end
puts "x".qa
