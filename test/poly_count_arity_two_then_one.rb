# poly count(x): a count(a, b) first (arity mismatch raises), a count(v) later
class Bag
  def count(a, b); :two; end
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
  def count(v); :one; end
end
xs.each { |o| p o.count(1) }
