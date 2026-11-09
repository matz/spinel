# spinel: gc-stress
# `str.split(sep) { |piece| }` and `str.scan(pat) { |m| }` with a receiver
# that builds a String (`.b`, a method's result) and a separator or pattern
# that allocates too: the receiver is held and rooted before the other is
# built, as the expression form holds it. C evaluates the call's arguments in no set order, and the one
# built first was nobody's while the other allocated, so the block saw no
# pieces, or pieces of a freed String read as invalid UTF-8.
def sep = "\xff".b
def text(n) = "x#{n}\xffy\xffz".b

r = []
"x\xffy".b.split("\xff".b) { |s| r << s.encoding }
p r.uniq, r.size
r = []
"a\xffb\xffc".b.split("\xff".b, 2) { |s| r << s }
p r
r = []
text(1).split(sep) { |s| r << s.bytesize }
p r
r = []
5.times { |i| text(i).split(sep) { |s| r << s } }
p r.size, r.last, r.map(&:encoding).uniq
r = []
"p,q".b.split(",".b) { |s| r << s.upcase }
p r

# scan with a block, the same way
r = []
5.times { |i| text(i).scan(sep) { |m| r << m } }
p r.size, r.map(&:encoding).uniq
