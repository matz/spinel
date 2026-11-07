# A table loop runs where the body reaches it (#10791): a core method it
# called and the program redefines later, Symbol#upcase here, is still the
# core's when the loop runs, as in CRuby. The scan over the whole program
# declined the loop because of the later definition.
module Tbl
  H = { a: 1, b: 2 }
  H.each { |k, v| const_set("K_#{k.upcase}", v) }
end
class Symbol
  def upcase = :changed
end
p Tbl::K_A, Tbl::K_B, :a.upcase
