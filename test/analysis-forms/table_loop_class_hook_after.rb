# A loop that makes a class runs where it stands: an inherited hook the
# body defines after it is not yet there, so only the later class calls it.
module Codes
  class Base; end
  Names = { 1 => "One" }
  Names.each { |c, n| const_set(n, Class.new(Base)) }
  class Base
    def self.inherited(k) = puts("inherited #{k.name}")
  end
  class Two < Base; end
end
p Codes::One.superclass, Codes::Two.superclass
