# A call in a block the record keeps (a Hash's default block) has a point of
# its own, but the block may run after any later statement: it meets the def
# made after it, with no exit handler in the program. A block that runs at
# once keeps the answer of its point.
class H; def baz = "baz"; end
h = Hash.new { |_, k| [k, H.new.baz] }
p h[0]
[1].each { p H.new.baz }
class H; def baz = "new"; end
p h[1]
[1].each { p H.new.baz }
class H; def baz = "newer"; end
p h[2], h[0]
