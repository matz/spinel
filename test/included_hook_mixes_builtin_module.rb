# An included hook that mixes a builtin module into the including class:
# extend Enumerable gives the class side sort, map and include? over its
# own each; include Comparable and Enumerable, the instance side.
module EnList
  def self.included(base)
    base.extend(Enumerable)
    base.include(Comparable)
    base.class_eval do
      include Enumerable
    end
  end
end
class EnBag
  include EnList
  def self.each
    yield 3
    yield 1
    yield 2
  end
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def each
    yield n
    yield n + 1
  end
end
p EnBag.sort, EnBag.map { |x| x * 2 }, EnBag.include?(2)
p EnBag.new(1) < EnBag.new(2), EnBag.new(5).to_a, EnBag.new(5).min
