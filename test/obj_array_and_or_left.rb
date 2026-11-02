# `x || y` and `x && y` with an Array of one class's objects on the left
# The left is a table that can be nil (`s&.items` on a missing Scan). The
# logical operator tests its truth as a pointer and boxes it where the
# result is boxed: it held it as always truthy, so `and return` returned on
# nil, and put the raw table beside a boxed right, which did not compile.
# An `Array.new` right where the chain's value is such a table is built as
# one: it was built as the general Array and read through the table's
# layout.

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
