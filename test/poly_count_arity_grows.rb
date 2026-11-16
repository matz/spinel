# poly count(x): a user count with one parameter defined after the first call
class Bag
  def initialize; @a = [1, 2, 1]; end
  def count; 99; end
end
xs = [[1, 1, 2], Bag.new]
xs.each do |o|
  begin
    p o.count(1)
  rescue ArgumentError => e
    p e.class
  end
end
class Bag
  def count(v); @a.count(v) + 100; end
end
xs.each { |o| p o.count(1) }
