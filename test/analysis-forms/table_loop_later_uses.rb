# A table loop's table used after the loop (#10791): CRuby has run the
# loop by then, so a later write to the table, at the top level or in a
# method called later, leaves what the loop set as it was.
module Tbl
  H = { a: 1, b: 2 }
  H.each { |k, v| const_set("K_#{k.upcase}", v) }
  H[:c] = 3
end

module Codes
  Names = { 200 => "OK", 404 => "Not Found" }
  Names.each_pair { |code, msg| const_set("RC_#{code}", msg) }
end
Codes::Names.delete(404)

def later = Tbl::H[:z] = 26
later

p Tbl::K_A, Tbl::K_B, Tbl::H
p Codes::RC_200, Codes::RC_404, Codes::Names
p Tbl.const_defined?(:K_C), Tbl.const_defined?(:K_Z)
