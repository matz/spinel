# A callable forwarded to an iterator on a value only known at run time
# answers what the iterator answers, at a method's tail too.

def sum_of(a, &b)
  return 0 unless b
  a.sum(&b)
end

def mapped(a, &b)
  if b then a.map(&b) else 0 end
end

def kept(a, &b)
  b ? a.select(&b) : 0
end

def sorted(a, &b)
  a.sort_by(&b) if b
end

def sorted_value(a, &b)
  x = a.sort_by(&b)
  x
end

def walked(a, &b)
  return 0 unless b
  a.reverse_each(&b)
end

def pairs(h, &b)
  if b then h.each_pair(&b) else 0 end
end

def least(a, &b) = a.min(&b)
def most(a, &b) = a.max(&b)
def once(a, &b) = a.uniq(&b)
def hashed(a, &b) = a.to_h(&b)
def entries(a, &b) = a.each_entry(&b)
def keys_of(h, &b) = h.each_key(&b)
def values_of(h, &b) = h.each_value(&b)

a = ARGV.size > 5 ? "x" : [3, 1, 2]
h = ARGV.size > 5 ? "x" : {1 => 2, 3 => 4}
double = proc { |v| v * 2 }
big = proc { |v| v > 1 }
seen = []
note = proc { |v| seen << v }
p sum_of(a, &double)
p mapped(a, &double)
p kept(a, &big)
p sorted(a, &double)
p sorted_value(a, &double)
def neg(v) = -v
p a.sort_by(&method(:neg))
p sorted_value(a, &method(:neg))
p walked(a, &note)
p seen
p pairs(h) { |k, v| seen << k + v }
p seen
by_mod = proc { |x, y| (x % 2) <=> (y % 2) }
p least(a, &by_mod)
p most(a, &by_mod)
p once(a) { |v| v % 2 }
p hashed(a) { |v| [v, v * 2] }
p entries(a, &note)
p keys_of(h, &note)
p values_of(h, &note)
p seen

class Integer
  def twice(&b) = b ? b.call(self) * 2 : :none
  def go(&b) = twice(&b)
  def bare = twice
end
p 3.go { |i| i + 1 }
p 3.go
p 3.bare
