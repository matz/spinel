# A negative count on a boxed receiver raises the message its own class
# does: take always "attempt to take negative size"; first by the receiver.
class V
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = V.new(n + 1)
  def inspect = "V#{n}"
end
def t
  yield
rescue => e
  p [e.class, e.message]
end
b = [V.new(1)..V.new(3), [1, 2], 1..3, "a".."c", [1, 2].each, (1..)]
b.each do |x|
  t { x.first(-1) }
  t { x.take(-1) }
  p x.take(1)
  p x.first(1)
end

# a Float Range checks the count before it says it cannot iterate
f = [1.0..2.0, [1]]
t { f[0].take(-1) }
t { f[0].first(-1) }
t { f[0].take(1) }
t { f[0].first(1) }
t { (1.0..2.0).take(-1) }
t { (1.0..2.0).first(-1) }
t { (1.0..2.0).take(1) }
t { (1.0..2.0).first(1) }
# an endless Range of objects too, where take used to walk first
t { (V.new(1)..).take(-1) }
t { [V.new(1).., 1][0].take(-1) }
p [V.new(1).., 1][0].take(2)

# a beginless Range's first refuses before the count; a Hash or a Time
# Range raises its own message, and take checks the count first
def tl(l)
  r = yield
  puts "#{l}: #{r.inspect}"
rescue => e
  puts "#{l}: #{e.class} #{e.message}"
end
t1 = Time.at(0)
b = [{a: 1}, (t1..t1 + 3), (..2.0), (..2), (1.0..), 1]
%w[hash trange bl_flt bl_int el_flt].each_with_index do |nm, i|
  x = b[i]
  tl("#{nm} first(-1)") { x.first(-1) }
  tl("#{nm} take(-1)") { x.take(-1) }
  tl("#{nm} first(nil)") { x.first(nil) }
  tl("#{nm} take(nil)") { x.take(nil) }
  tl("#{nm} first(1)") { x.first(1) }
end
tl("typed bl_flt first(-1)") { (..2.0).first(-1) }
tl("typed bl_flt take(-1)") { (..2.0).take(-1) }
tl("typed bl_flt first(nil)") { (..2.0).first(nil) }
tl("typed el_flt first(-1)") { (1.0..).first(-1) }
tl("typed el_flt take(-1)") { (1.0..).take(-1) }
tl("typed bl_int first(-1)") { (..2).first(-1) }
tl("typed bl_int first(1)") { (..2).first(1) }
tl("typed int first(-1)") { (1..3).first(-1) }
tl("typed hash first(-1)") { {a: 1}.first(-1) }
