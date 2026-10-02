# A boxed Hash (and Array) beside classes of the program that define the
# same method names: the call dispatches on the class, and the Hash still
# answers as a Hash
class Bag
  def flatten(*a) = :bag
  def compact(*a) = :bag
  def to_h(*a, &b) = :bag
  def replace(*a) = :bag
  def transform_values!(*a, &b) = :bag
  def sort = yield(1, 2)
  def select! = yield(1)
  def transform_values(*a, &b) = :bag
  def sum(*a, &b) = :bag
  def fetch(*a, &b) = :bag
  def delete(*a, &b) = :bag
end
bag = Bag.new
p bag.flatten, bag.compact, bag.to_h, bag.replace(1), bag.transform_values!(1), bag.sum(1), bag.fetch(1), bag.delete(1)
boxed_bag = [bag, [1]][0]
p boxed_bag.sort { |x, y| x + y }, boxed_bag.select! { |x| x * 5 }

v = [{ "a" => 1, "n" => nil }, 1][0]
p v.flatten, v.compact, v.to_h { |k, x| [k, 1] }, v.transform_values { |x| x.to_s }
p v.sum([]) { |k, x| [k] }, v.fetch("z") { |k| k * 2 }, v.fetch("a")
w = v.dup
w.replace("r" => 1)
p w
w = v.dup
w.transform_keys!(&:upcase)
p w
w = v.dup
p w.delete("zz") { |k| k }, w.delete("a") { |k| k }, w
a = [[1, nil, [2]], 1][0]
p a.flatten, a.compact, a.sum(0) { |x| x.is_a?(Integer) ? x : 0 }

# with no class of the name: merge! with a block, delete with a block, to_hash
u = [{ "a" => 1 }, 1][0]
u.merge!("a" => 2) { |k, o, n| o + n * 10 }
p u, u.to_hash, u.delete("q") { |k| "no #{k}" }

# a typed Hash beside the same names still transforms in place
h = { "a" => 1, "b" => 2 }
h.transform_values! { |x| x * 10 }
p h
f = proc { |k, x| [x, k] }
p v.to_h(&f)

# the in-place filters, beside a class that yields for them
class Sieve
  def select! = yield(1)
  def keep_if = yield(2)
  def reject! = yield(3)
end
sv = [Sieve.new, 1][0]
p sv.select! { |x| x * 10 }, sv.keep_if { |x| x + 1 }, sv.reject! { |x| x }
arr = [[1, 2, 3, 4], 1][0]
p arr.select! { |x| x.even? }, arr, arr.select! { |x| true }
p arr.keep_if { |x| x > 2 }, arr.reject! { |x| x > 9 }, arr.reject! { |x| x == 4 }, arr
hh = [{ "a" => 1, "b" => 2 }, 1][0]
p hh.select! { |k, x| x > 1 }, hh, hh.keep_if { |k, x| true }, hh.reject! { |k, x| false }
dd = [[1, 1, 2, 1], 1][0]
p dd.delete_if { |x| x == 1 }, dd

# blocks handed to the builtin beside a class whose same-name method yields
class Yielder
  def merge!(h) = yield(1, 2, 3)
  def fetch(*a) = yield(1)
  def delete(*a) = yield(1)
end
p Yielder.new.merge!(1) { |a, b, c| a + b + c }, Yielder.new.fetch(1) { |a| a + 1 }, Yielder.new.delete(1) { |a| a + 1 }
yy = [{ "a" => 1 }, 1][0]
p yy.merge!("a" => 2) { |k, o, n| o + n }, yy.fetch("zz") { |k| k + "!" }, yy.delete("zz") { |k| k + "!" }

# a break keeps what went before it; a frozen receiver refuses first
bh = [{ "a" => 1, "b" => 2, "c" => 3 }, 1][0]
p bh.select! { |k, x| break :brk if x == 2; x != 1 }, bh
begin
  [[1, 2].freeze, 1][0].select! { true }
rescue FrozenError => e
  p e.class
end


# blocks given as procs, and break / return in them
h = [{"a" => 1, "b" => 2}, 1][0]
pr = proc { |k, o, n| o * n }
p h.update({"a" => 5}, &pr), h.merge!({"b" => 3}) { |k, o, n| o + n }
class U2
  def fetch(*a) = yield(1)
  def filter!(*a) = yield(1)
  def merge!(*a) = yield(1, 2, 3)
end
p U2.new.fetch { |x| x }, U2.new.filter! { |x| x }, U2.new.merge!(1) { |a, b, c| a + b + c }
g = [{"a" => 1, "b" => 2}, [3, 1, 2], U2.new][0]
p g.fetch("zz", &proc { |k| "#{k}!" }), g.fetch("a") { |k| 0 }
p g.fetch("q") { |k| break :fb }
def m(h) = h.fetch("q") { return :fr }
p m(g)
ar = [[3, 1, 2], 1][0]
p ar.fetch(9, &proc { |i| i * 2 }), ar.fetch(-1) { 0 }
p g.merge!({"a" => 10}, &pr), g.update({"c" => 1}) { |k, o, n| 0 }
w = [(1..2), U2.new][0]
p((w.filter! { |x| x } rescue $!.message))

# a Float index for fetch's block, several Hashes for merge!/update's
a = [[10, 20], 1][0]
p a.fetch(1.7) { |i| [:blk, i] }, a.fetch(5.5) { |i| [:blk, i] }, a.fetch(-1.2) { :x }
h = [{"a" => 1}, 1][0]
p h.update({"a" => 2}, {"a" => 3, "b" => 4}) { |k, o, n| o + n }
class U3
  def update(*a) = yield(1, 2, 3)
  def fetch(*a) = yield(1)
end
p U3.new.update(1) { |a, b, c| a + b + c }, U3.new.fetch(1) { |x| x }
g = [{"a" => 1}, U3.new][0]
p g.update({"a" => 2}, {"a" => 3}) { |k, o, n| o * n }
ar = [[10, 20], U3.new][0]
p ar.fetch(9.9) { |i| i }

# a nested call among merge!'s Hashes; a block argument that is nil
h1 = [{ "q" => 1 }, 1][0]
y = [{ "a" => 1 }, 1][0]
p y.update(h1.update({ "q" => 5 }) { |k, o, n| n }, { "z" => 2 }) { |k, o, n| n }
blk = nil
p y.fetch("a", &blk), y.update({ "a" => 9 }, &blk)
