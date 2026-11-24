# A String the loop computes is a new, unfrozen object in CRuby, so
# `M::N1 << "!"` works there. The analysis writes it as a new unfrozen
# String.

module M
  class Base; end
  T = { 1 => "O ne" }
  T.each { |c, n| const_set("N#{c}", n.gsub(/ /, "")); const_set(n.gsub(/ /, ""), Class.new(Base)) }
end
M::N1 << "!"
p M::N1
p M::One.superclass
