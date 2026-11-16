# Two captures in one statement meet what main's chain holds at each: a def in an argument between them gives
# the second main's own method, and the first stays Kernel's.
r = [method(:puts), (def self.puts(*a) = "own"), method(:puts)]
r[0].call("a")
p r[2].call("b")
