# A program's Enumerable method of a name builtins/enumerable.rb has no
# definition of (map, uniq, zip, sort, ...) answers a Hash, a Range and an
# Enumerator, which have no method of that name. An Array's own method of
# the name still answers first.
module Enumerable
  def map(*a) = [:map, a.size]
  def uniq(*a) = :uniq
  def zip(*a) = :zip
  def sort(*a) = :sort
  def sort_by(*a) = :sort_by
  def min(*a) = :min
  def max(*a) = :max
  def first(*a) = :first
  def sum(*a) = :sum
  def find_all(*a) = :find_all
  def reverse_each(*a) = :reverse_each
  def each_entry(*a) = :each_entry
  def each_slice(*a) = :each_slice
  def chain(*a) = :chain
  def to_set(*a) = :to_set
  def take(*a) = :take
end

h = {a: 1, b: 2}
p h.map { |k, v| v }
p h.uniq { |x| x }
p h.zip([1])
p h.sort
p h.sort_by { |k, v| v }
p h.min
p h.max { |a, b| a <=> b }
p h.first
p h.sum
p h.find_all { true }
p h.reverse_each { }
p h.each_entry { }
p h.each_slice(1) { }
p h.chain
p h.to_set
p h.take(1)

r = (1..3)
p r.map { |x| x }
p r.uniq
p r.zip([1])
p r.sort
p r.sort_by { |x| -x }
p r.find_all { true }
p r.sum
p r.each_entry { }
p r.chain
p r.to_set
p r.min
p r.first
p r.take(1)

e = [1, 2].each
p e.map { |x| x }
p e.uniq
p e.sort
p e.each_slice(1) { }
p e.find_all { true }

a = [3, 1, 3]
p a.map { |x| x * 2 }
p a.uniq
p a.zip([1])
p a.sort
p a.min
p a.first
p a.sum
p a.take(1)
p a.sort_by { |x| x }
p a.find_all { |x| x > 1 }

class Bag
  include Enumerable
  def each
    yield 2
    yield 1
  end
end
p Bag.new.map { |x| x }
p Bag.new.sort
