# A kept block that writes an INDEX on its parameter (`acc[:k] = 1`, a Hash
# write) is not "a proc that appends to the String it is handed". `[]=` is on
# the String-mutator table for `s[0] = "a"`, but a Symbol index, or a value
# written as a literal of another class, cannot be String#[]=. Counting it as
# one refused every unresolved String `yield` in the program -- here `each`
# below -- for a String nothing touched.
class Bag
  include Enumerable

  def initialize
    @items = ["a", "b"]
  end

  def each
    @items.each { |x| yield x }
    self
  end
end

def keep(&blk)
  blk
end

# `to_a` reaches `each` through Enumerable: the yield's block is not a literal
# at this call, which is the "unresolved yield" the refusal was raised for.
p Bag.new.to_a

writer = keep { |acc| acc[:k] = 1 }
h = { z: 0 }
writer.call(h)
p h

# The memo of `each_with_object({})` is the Hash the call names, whatever the
# key and value are -- here an Integer key and a String value, which a String
# receiver would also accept, so only the memo's own spelling settles it.
labels = [3, 4].each_with_object({}) do |n, map|
  map[n] = "n" + n.to_s
end
p labels
