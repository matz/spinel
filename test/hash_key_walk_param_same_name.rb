# An empty Hash keyed by the parameter of a walk over a String Array takes the
# String-keyed variant, even where the parameter read boxed for a round.
#
# `L#titles` maps over an Array of T and calls `t.title`. With a second method
# named `title` in the program (`A.title`), that call has two candidates, so
# on the round where the block parameter `t` still read boxed (an object
# array's walk binds it boxed until it narrows) the call answered boxed, and
# `titles` was an Array of boxed values for that round. The parameter `s` of
# the walk over it widened to poly (block parameters only widen during the
# fixpoint), and the key pass took that stale poly and pinned `h = {}` to the
# poly-keyed sp_PolyPolyHash for good -- even though `titles` then settled to
# a String Array and the re-narrow re-typed `s` as a String. Renaming
# `A.title` made it sp_StrIntHash, and an RBS return of Hash[String, Integer]
# changed nothing. The key pass now asks the walk's receiver: a typed Array
# keys the element type, and a receiver that still reads boxed is waited for.
# `Store#tags` over `Item#tags` is the same shape through flat_map, whose
# receiver settled only after the fixpoint's pessimistic stage began.
#
# A walk over an Array that really is mixed still stores every key: the
# store widens the Hash when the types settle.
class T
  attr_reader :title
  def initialize(t) = @title = t
end
class L
  def initialize = @items = [T.new("a"), T.new("b"), T.new("a")]
  def titles = @items.map { |t| t.title }
end
module A
  #: (L) -> Hash[String, Integer]
  def self.w(l)
    h = {}
    l.titles.each { |s| h[s] = 1 }
    h
  end
  #: (T) -> String
  def self.title(t) = t.title
end
p A.w(L.new)
puts A.title(T.new("c"))

class Item
  attr_reader :tags
  def initialize(tags) = @tags = tags
end
class Store
  def initialize = @items = [Item.new(["x", "y"]), Item.new(["y"])]
  def tags = @items.flat_map { |i| i.tags }
end
module TodoApp
  def self.tag_counts(store)
    h = {}
    store.tags.each { |t| h[t] = (h[t] || 0) + 1 }
    h
  end
end
p TodoApp.tag_counts(Store.new)

def mixed = [1, "a", 1, :b]
def mixed_counts
  h = {}
  mixed.each { |k| h[k] = (h[k] || 0) + 1 }
  h
end
p mixed_counts
seen = {}
mixed.each { |k| (seen[k] ||= []) << k }
p seen
