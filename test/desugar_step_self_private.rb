# A call written on self may reach a private method, also where a desugar
# lowers it through a step of its own (`self.to_h { }` becomes a map and a
# to_h, `self.min(2) { }` a sort and a first): the call keeps the permission
# it was written with. The same call on another receiver raises.

class Array
  private :to_h, :min
  def pairs = self.to_h { |v| [v, v * 2] }
  def pairs2 = to_h { |v| [v, v + 1] }
  def low2 = self.min(2) { |a, b| a <=> b }
end

class Hash
  private :to_h
  def flip = self.to_h { |k, v| [v, k] }
end

class Range
  private :to_h
  def squares = self.to_h { |v| [v, v * v] }
end

p [1, 2].pairs
p [1, 2].pairs2
p [3, 1, 2].low2
p({ a: 1 }.flip)
p (1..3).squares
begin
  p [1, 2].to_h { |v| [v, v] }
rescue NoMethodError => e
  puts "outside: #{e.class}"
end
