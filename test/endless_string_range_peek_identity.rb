# spinel: gc-minor
# Peeking an endless String range holds the same item that next returns.
source = +"a"
walk = (source..).step(2)
first = walk.peek
first << "!" * 256
following = walk.next
p [first.equal?(following), following.bytesize, walk.next, source]
symbols = (:a..).step(2)
p [symbols.peek, symbols.next, symbols.next]
