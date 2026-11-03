# respond_to? on a String, Array or Hash slot that may hold nil
# A typed slot that may hold nil holds it as NULL. The answer for a
# builtin receiver came from the type alone (the typed probe call), so a
# nil in the slot answered as the Array, String or Hash would: true for
# :each, :upcase or :each_pair, where nil answers only its own surface.
# An object slot already answered by its non-nilness; these now do too,
# as a value and as a branch condition.
Item = Struct.new(:id, :w)
class Scan
  attr_reader :items, :name, :meta
  def initialize(n)
    @items = Array.new(n) { |i| Item.new(i, i) }
    @name = "s#{n}"
    @meta = { n: n }
  end
end
class Reader
  def initialize(k) = @scans = Array.new(k) { |i| Scan.new(i + 2) }
  def t = @scans[0]&.items
  def nm = @scans[0]&.name
  def mt = @scans[0]&.meta
end
[Reader.new(2), Reader.new(0)].each do |r|
  t = r.t
  p [t.respond_to?(:each), t.respond_to?(:to_a), t.respond_to?(:inspect), t.respond_to?(:zork)]
  p [r.nm.respond_to?(:upcase), r.nm.respond_to?(:to_s), r.mt.respond_to?(:each_pair)]
  if t.respond_to?(:each)
    puts "iterable"
  else
    puts "not iterable"
  end
  puts(r.t.respond_to?(:size) ? "sized" : "unsized")
end
a = [1, 2]
p a.respond_to?(:each), "x".respond_to?(:upcase)
