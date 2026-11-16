# the builtins answer before they reach the undefined <=>: the same object, clamp's bounds
class M1
  include Comparable
  attr_reader :v
  def initialize(v = 0); @v = v; end
  def <=>(o); v <=> o.v; end
end
class M2 < M1
  undef_method :<=>
end
m = M2.new
p(m == m)
p(m != m)
p(m === m)
p(m.clamp(nil, nil).equal?(m))
begin
  p(m == M2.new)
rescue NoMethodError => e
  puts e.message
end
begin
  p(M2.new(1).clamp(M1.new(5), M1.new(2)))
rescue ArgumentError => e
  puts e.message
end
begin
  p(m.clamp(M1.new(1), M1.new(2)))
rescue NoMethodError => e
  puts e.message
end
p(m.clamp(nil..nil).equal?(m))
begin
  p(m.clamp(2..1))
rescue ArgumentError => e
  puts e.message
end
begin
  p(m.clamp(1...2))
rescue ArgumentError => e
  puts e.message
end
begin
  p(m.clamp(1..2))
rescue NoMethodError => e
  puts e.message
end

# a Comparable class that undefines == raises there, for == != and ===
class E1
  include Comparable
  attr_reader :v
  def initialize(v = 0); @v = v; end
  def <=>(o); v <=> o.v; end
end
class E2 < E1
  undef_method :==
end
q = E2.new(1)
begin; p(q == q); rescue NoMethodError => x; puts x.message; end
begin; p(q != q); rescue NoMethodError => x; puts x.message; end
begin; p(q === E2.new(1)); rescue NoMethodError => x; puts x.message; end
begin; p(q != E2.new(2)); rescue NoMethodError => x; puts x.message; end
