# A method the program defines in Range has a body for an Integer Range
# alone: on a Range of objects the call walked the members as an Array and
# answered Array#map (CRuby prints :overridden), so it is refused instead.
class V
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = V.new(n + 1)
end
class Range
  def map
    :overridden
  end
end
p (V.new(1)..V.new(2)).map { |v| v.n }
