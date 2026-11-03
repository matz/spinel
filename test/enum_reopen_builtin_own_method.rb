# A program's `module Enumerable` method is behind a builtin class's own
# method of the name: CRuby looks the class up first. An Array has its own
# find_index, count, take_while and more; a Hash its own any?; an
# Enumerator its own each_with_index. A Range calls super for count and
# minmax with a block, so the module's method answers it.
module Enumerable
  def count(*a) = :e_count
  def any?(*a) = :e_any
  def all?(*a) = :e_all
  def none?(*a) = :e_none
  def one?(*a) = :e_one
  def find(*a) = :e_find
  def find_index(*a) = :e_fi
  def take_while = :e_tw
  def drop_while = :e_dw
  def cycle(*a) = :e_cycle
  def minmax = :e_minmax
  def each_with_index = :e_ewi
  def each_with_object(m) = :e_ewo
  def tally = :e_tally
end

class Bag
  include Enumerable
  def each; yield 1; yield 2; end
end

a = [1, 2, 3]
h = {a: 1, b: 2}
r = (1..3)
b = Bag.new

p a.count { |x| x > 1 }, h.count { |k, v| v > 1 }, r.count { |x| x > 1 }, b.count { |x| x > 1 }
p a.any? { |x| x > 1 }, h.any? { |k, v| v > 1 }, r.any? { |x| x > 1 }, b.any? { |x| x > 1 }
p a.all? { |x| x > 0 }, h.all? { |k, v| v > 0 }, r.all? { |x| x > 0 }
p a.none? { |x| x > 5 }, a.one? { |x| x > 2 }, h.none? { |k, v| v > 5 }
p a.find { |x| x > 1 }, a.detect { |x| x > 1 }, h.find { |k, v| v > 1 }
p a.find_index { |x| x > 1 }, h.find_index { |k, v| v > 1 }, r.find_index { |x| x > 1 }, b.find_index { |x| x > 1 }
p a.take_while { |x| x < 2 }, a.drop_while { |x| x < 2 }, h.take_while { |k, v| v < 2 }, r.take_while { |x| x < 2 }
p a.cycle(1) { |x| x }, h.cycle(1) { |x| x }
p a.minmax { |x, y| x <=> y }, r.minmax { |x, y| x <=> y }, h.minmax { |x, y| x <=> y }
p a.minmax, r.minmax
p a.each_with_index { |x, i| x }, a.each.each_with_index { |x, i| x }, h.each_with_index { |x, i| x }
p a.each_with_object([]) { |x, m| m << x }, r.each_with_object([]) { |x, m| m << x }
p a.tally, h.tally

# an Array subclass has Array's methods
class Sub < Array; end
p Sub.new([4, 2]).find_index { |v| v == 2 }

# a value known only at run time
def fi(x) = x.find_index { |v| v == 2 }
def tw(x) = x.take_while { |v| true }
def an(x) = x.any? { |*v| true }
def mm(x) = x.minmax { |p, q| p <=> q }
def ct(x) = x&.count { |*v| true }
p fi([1, 2]), fi({a: 1}), fi(1..3)
p tw([1, 2]), tw({a: 1}), tw(1..3)
p an([1]), an({a: 1}), an(1..2), an(Bag.new)
p mm([2, 1]), mm(1..2), mm({a: 1})
p ct([1, 2]), ct({a: 1}), ct(1..2), ct(nil)
