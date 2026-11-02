# A table read through a reader on an element of another table is vetted
# `scans[0].items` reads Scan's @items through an element of `scans`, a
# table the compiler narrows to Scans. The reader call is decided once that
# class is known, and was then taken as no use of @items at all: @items
# narrowed to an unboxed table although its value went on to calls no such
# table answers. `nil?`, `==`, `is_a?`, `frozen?` and `to_a` were refused,
# `!t` and `case t when nil` answered as if nil were a table, and
# `@cur ||= ...` dropped its store. The read is now a use of @items like
# any other, so @items stays a general Array here.

Item = Struct.new(:id, :w)

class Scan
  attr_reader :items

  def initialize(n) = @items = Array.new(n) { |i| Item.new(i, i) }
end

class Reader
  def initialize(k)
    @scans = Array.new(k) { |i| Scan.new(i + 2) }
    @cur = nil
  end

  def t = @scans[0]&.items
  def cur = (@cur ||= @scans[1].items)
end

scans = Array.new(2) { |i| Scan.new(i + 2) }
t = scans[0].items
p t.nil?, t.frozen?, t.is_a?(Array), t == scans[0].items, t.to_a.size

[Reader.new(2), Reader.new(0)].each do |r|
  u = r.t
  p u.nil?, !u, (u ? :table : :none)
  p(case u when nil then :nil when Array then :array end)
end
p Reader.new(3).cur.map(&:w)
