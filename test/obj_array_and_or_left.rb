# `x || y` and `x && y` with an Array of one class's objects on the left
# The left is a table that can be nil (`s&.items` on a missing Scan). The
# logical operator tests its truth as a pointer and boxes it where the
# result is boxed: it held it as always truthy, so `and return` returned on
# nil, and put the raw table beside a boxed right, which did not compile.

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
end
