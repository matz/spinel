# A block a method takes as its &block and answers reports its parameters
# with their kinds and names, beside another call that hands one on.
def keep(&block) = block
a = keep { |row, token:| token }
b = keep { |row, title:| title }
p a.parameters
p b.parameters
BODIES = {}
def op(name, &block) = BODIES[name] = block
op(:a) { |row, token:| token }
op(:b) { |x, y = 1, *r, k: 2, **o, &blk| x }
op(:c) { _1 + _2 }
op(:d) { |(p, q), z| z }
BODIES.each { |name, block| p [name, block.parameters, block.arity] }
p keep { |s| s }.parameters, keep { it }.parameters
