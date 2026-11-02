# `x || y` and `x && y` with a table read through an element's reader
# `@scans[0]&.items` reads Scan's @items through an element of @scans, a
# table being narrowed. That read was no use of @items the narrowing saw,
# so @items narrowed to an unboxed table, which reached `||` and `&&`:
# the logical operators took it as always truthy (`and return` returned on
# nil) and put it raw beside a boxed right, which did not compile, and an
# `Array.new` beside it was built as the general Array. The read is now a
# use of @items, which stays a general Array, and each form answers as it
# does in CRuby.

Item = Struct.new(:id, :w)

class Scan
  attr_reader :items

  def initialize(n) = @items = Array.new(n) { |i| Item.new(i, i) }
end

class Reader
  def initialize(k) = @scans = Array.new(k) { |i| Scan.new(i + 2) }

  def or_empty = @scans[0]&.items || []
  def or_nil = @scans[0]&.items || nil
  def and_value = @scans[0]&.items && 7
  def or_new = @scans[0]&.items || Array.new

  def or_return
    (@scans[0]&.items or return 0).size
  end

  def and_return
    @scans[0]&.items and return 1
    2
  end
end

[Reader.new(2), Reader.new(0)].each do |r|
  p r.or_empty.size, r.or_nil&.size, r.and_value
  p r.or_return, r.and_return
  t = r.or_new
  t << Item.new(7, 70)
  p t.map(&:w), t.last.w
end
