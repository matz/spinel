$c = 0
def cap; $c += 1; 5; end
# A Hash subclass's super(capacity: n) in initialize keeps the entries
# written ahead of it and sets the default or default proc it passes;
# a capacity value that runs code still runs once
class H1 < Hash
  def initialize; self[:x] = 1; super(capacity: 10); end
end
p H1.new
class H2 < Hash
  def initialize; self[:x] = 1; super(0, capacity: 10); end
end
h = H2.new; p h, h[:nope]
class H3 < Hash
  def initialize; self[:y] = 2; super(capacity: 4) { |hh, k| k.to_s }; end
end
h = H3.new; p h, h[:z]
class H4 < Hash
  def initialize(n); self[:n] = n; super(capacity: n); end
end
p H4.new(3)
class H5 < Hash
  def initialize; super(capacity: 10); self[:a] = 1; end
end
p H5.new, H5.new.size
class H6 < Hash
  def initialize(d); super(d, capacity: 2); end
end
p H6.new(7)[:q]
class H10 < Hash
  def initialize(a); a.each { |k, v| self[k] = v }; super(nil, capacity: a.size); end
end
p H10.new({a: 1, b: 2})
class H7 < Hash
  def initialize; self[:x] = 1; super(capacity: cap); end
end
p H7.new, $c
class H12 < Hash
  def initialize; self[:x] = 1; super(:d, capacity: cap); end
end
h = H12.new; p h, h[:zz], $c
class H13 < Hash
  def initialize; self[:x] = 1; super(capacity: cap) { |hh, k| k.to_s * 2 }; end
end
h = H13.new; p h, h[:ab], $c
class H14 < Hash
  def initialize; self[:x] = 1; super(capacity: "no"); end
end
begin; p H14.new; rescue TypeError => e; p e.class; end
class H15 < Hash
  def initialize; self[:x] = 1; super(capacity: cap); end
end
h = H15.new { |hh, k| k.to_s + "!" }; p h, h[:q], $c
class H16 < Hash
  def initialize; self[:x] = 1; super(capacity: 3); end
end
h = H16.new { |hh, k| k.to_s + "?" }; p h, h[:q]
class H17 < H16; end
p H17.new
