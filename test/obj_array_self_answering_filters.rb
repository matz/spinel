# delete_if, keep_if, sort! and sort_by! answer the receiver itself, never
# nil, so an Array of one class read through their value stays narrowed
# (sp_PtrArray): the value is the same Array. They used to be read like
# reject!, select! and filter!, which answer the Array or nil, and narrowed
# only where their value was thrown away: `def drop(n) = @items.delete_if
# { ... }` left @items boxed wherever drop's value was read, an --ext-entry's
# return among them. infer-test checks that Shelf, Bin and Queue2 keep
# @items an sp_PtrArray; Pick (select!) stays boxed.
class Item
  attr_reader :name, :rank
  def initialize(name, rank)
    @name = name
    @rank = rank
  end
end

class Shelf
  def initialize = @items = [Item.new("pear", 3), Item.new("fig", 1), Item.new("kiwi", 2)]
  def drop(n) = @items.delete_if { |i| i.name == n }
  def names = @items.map { |i| i.name }
end

class Bin
  def initialize = @items = [Item.new("a", 5), Item.new("b", 0), Item.new("c", 7)]
  def keep_ranked = @items.keep_if { |i| i.rank > 0 }
  def names = @items.map { |i| i.name }
end

class Queue2
  def initialize = @items = [Item.new("x", 9), Item.new("y", 4), Item.new("z", 6)]
  def by_rank = @items.sort_by! { |i| i.rank }
  def by_name_desc = @items.sort! { |a, b| b.name <=> a.name }
  def names = @items.map { |i| i.name }
end

# select! answers nil when nothing is removed: still read as the boxed Array
class Pick
  def initialize = @items = [Item.new("m", 1), Item.new("n", 2)]
  def keep_big = @items.select! { |i| i.rank > 1 }
end

s = Shelf.new
kept = s.drop("fig")
p kept.size
p kept.map { |i| i.rank }
p s.names
p s.drop("none").size
p s.drop("pear").map { |i| i.name }
p s.names

b = Bin.new
p b.keep_ranked.map { |i| i.name }
p b.names

q = Queue2.new
p q.by_rank.map { |i| i.name }
p q.by_name_desc.first.name
p q.names

k = Pick.new
r = k.keep_big
p r.nil?
p k.keep_big.nil?
