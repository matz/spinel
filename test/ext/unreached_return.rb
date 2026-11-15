# An --ext-entry has no call site in the program: the host calls it, through
# the header written from the types the analysis leaves. Its return slot was
# killed all the same, as a method entered by a route the narrowing never
# saw, and `add`'s value (the push answers @items) joined that slot to
# @items: the ivar stayed a boxed poly array, and every read of an element
# went through the boxed dispatch. ext-test checks that @items is an
# sp_PtrArray.
class Item
  attr_reader :id, :done
  def initialize(id, done) = (@id = id; @done = done)
end

class Store
  attr_reader :items
  def initialize = @items = []
  def add(id, done) = @items << Item.new(id, done)
  def open = @items.reject { |i| i.done }
end

module ExtUnreached
  #: () -> Store
  def self.mk = Store.new
  #: (Store, Integer, bool) -> void
  def self.add(s, id, done) = s.add(id, done)
  #: (Store) -> Integer
  def self.open_ids(s) = s.open.sum { |i| i.id }
end
