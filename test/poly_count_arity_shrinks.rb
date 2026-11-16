# poly count(x): the class's count takes one argument first, none after a reopen
class Bag
  def initialize; @a = [1, 2, 1]; end
  def count(v); @a.count(v) + 100; end
end
xs = [[1, 1, 2], Bag.new]
xs.each { |o| p o.count(1) }
class Bag
  def count; 99; end
end
xs.each do |o|
  begin
    p o.count(1)
  rescue ArgumentError => e
    p e.class
  end
end
