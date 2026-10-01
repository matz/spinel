# A block's locals are fresh on every call, also when the yield that runs
# it is read for its value before any other block body was emitted: the
# splice found the block through a map built on first use, and read
# straight off the compiler it was not built yet, so `d` kept the previous
# call's value.

def y2 = [yield(1), yield(2)]
p(y2 { |q| next 0 if d = !d; q })
p(y2 { |q| d = !d; next 0 if d; q })
p(y2 { |q| d = !d; d ? 0 : q })

def y3; a = yield(1); b = yield(2); [a, b]; end
p(y3 { |q| t = (t || 0) + q; t })

def y4 = 2.times.map { |i| yield(i) }
p(y4 { |q| d = !d; d ? 0 : q })
p([3, 1].count { |q| next true if d = !d; false })
p([3, 1].find_index { |q| next false if d = !d; true })
