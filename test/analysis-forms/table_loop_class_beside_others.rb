# The classes a loop makes are the module's own: a class of the same leaf
# in another module, or at the top level, is another class.
module Codes
  class Base; end
  Names = { 1 => "One", 2 => "Two" }
  Names.each { |c, n| const_set(n, Class.new(Base)) }
end
module Other
  class One
    def self.tag = :other
  end
end
class One
  def self.tag = :top
end
p Codes::One.superclass, Codes::Two.name, Other::One.tag, One.tag, Codes::One == One
