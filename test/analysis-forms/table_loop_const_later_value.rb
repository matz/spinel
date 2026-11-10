# A value given after a table loop to a constant the loop set (#10791):
# CRuby has run the loop by then, and the later value replaces the loop's,
# by a write or a const_set; another module's constant of the same leaf is
# its own.
module Tbl
  H = { a: 1, b: 2 }
  H.each { |k, v| const_set("K_#{k.upcase}", v) }
end
Tbl.const_set(:K_B, 7)
module Tbl
  K_A = 5
  def self.both = [K_A, K_B]
end
module Other
  K_A = 9
end
p Tbl::K_A, Tbl::K_B, Tbl.both, Other::K_A
