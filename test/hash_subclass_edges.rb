# Hash subclasses: the class line found by parsing (not in a heredoc), the
# nested ::Hash form, dig, freeze, each_with_index, Enumerable
src = <<~RUBY
  class Foo < Hash
  end
RUBY
puts src
class Bar < Hash # trailing
end
module Rack
  class Headers2 < ::Hash; end
end
b = Bar.new; b[:k] = 1
h = Rack::Headers2.new; h["x"] = 2
p b, h, h.class

class Dg < Hash
end
x = Dg.new
x[:n] = { m: { o: 1 } }
x[:a] = [10, [20, 30]]
p x.dig(:n, :m, :o), x.dig(:n, :q, :o), x.dig(:a, 1, 0), x.dig(:zz)
v = [{ m: 1 }, 2][0]
p v.dig(:m)

class Fz < Hash
end
fz = Fz.new
fz["a"] = 1
p fz.freeze.equal?(fz), fz.frozen?
begin
  fz["c"] = 2
rescue => e
  p e.class, e.message
end
begin
  fz.update("d" => 1)
rescue FrozenError => e
  p e.class
end
p fz, fz.dup.frozen?

class Nb < Hash
end
nb = Nb.new
nb["a"] = 1
nb["b"] = 2
p nb.each_with_index.to_a, nb.each_with_index { |(k, v), i| }.class
p nb.kind_of?(Enumerable), nb.is_a?(Hash), Nb.included_modules.include?(Enumerable)

# freeze reaches Object's through super (the object too), and a boxed object sums
class Fz2 < Hash
end
hz = Fz2.new
hz["a"] = 1
hz.freeze
p hz.frozen?
begin
  hz.instance_variable_set(:@x, 1)
rescue => e
  p e.class
end
class Su
  def sum(*a, &b) = :u
end
Su.new.sum(1) { }
w = [Fz2.new.tap { |x| x["a"] = 3; x["b"] = 4 }, Su.new][0]
p w.sum(0) { |k, v| v }

class Pf
  def freeze
    @x = 1
    super
  end
end
class Pg
  def freeze
    r = super
    r
  end
end
a = [Pf.new, Pg.new, 1]
p a[0].freeze.frozen?, a[1].freeze.class, Pf.new.freeze.frozen?
