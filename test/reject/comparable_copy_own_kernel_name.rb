# A method added to `module Comparable` calls p, format or puts with no
# receiver. On a String or a Symbol, CRuby calls the program's own String#p
# (or a module's, Object's or a top-level one) over Kernel's. Spinel's copy
# of the Comparable method for a String calls Kernel's, so it printed 1
# where CRuby prints S1. Refused, as before the copy came in (#11170).
class String
  def p(x) = "S#{x}"
end
module Comparable
  def qa = p(1)
end
puts "x".qa
