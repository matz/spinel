# A method that forwards its own block to an Enumerable call
# (`def drop_while(&blk) = @items.drop_while(&blk)`), reached through a
# send whose name is known only at run time on a value of one of several
# classes: the dispatch calls the method's proc form, handed the send's
# block or none. Whether that block is there is known only when it runs.
# Taken for given, a send with no block yielded to none (LocalJumpError);
# taken for absent where every call of the method gave none, the form's
# block arm did not fit the blockless arm's Enumerator, and the C did not
# compile.
class Bag
  def initialize(e) = @items = e
  def drop_while(&blk) = @items.drop_while(&blk)
  def size = @items.size
end

class Box
  def initialize(e) = @e = e
  def drop_while(&blk) = @e.drop_while(&blk)
  def size = 0
end

items = [Bag.new([1, "two", 3]), Box.new([4, 5])]
names = %w[size drop_while].map { |s| s + "" }
items.each do |it|
  names.each { |n| p it.public_send(n.to_sym).class }
end
items.each do |it|
  names.each { |n| p it.public_send(n.to_sym) { |x| x == 1 } }
end
p items.map { |it| it.drop_while { |x| x == 4 } }
