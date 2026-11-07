# A table loop's const_set is the one a call on the body's own class
# reaches at the loop's point (#10791): another class's `def
# self.const_set`, defined after it, does not change what the loop does.
module Tbl
  H = { a: 1 }
  H.each { |k, v| const_set("K_#{k.upcase}", v) }
end
class Box
  def self.const_set(n, v) = :mine
end
p Tbl::K_A, Box.const_set(:X, 1)
