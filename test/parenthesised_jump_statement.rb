# A statement in parentheses that ends in a return, a break or a next runs
# as the statement it is: `(return v) if c`, a ternary's `(return v)` arm,
# `(break) if c` in a loop.
def pick(n, v)
  (return v) if n == 1
  0
end
p pick(1, 5)
p pick(2, 5)
def tern(n, v)
  n == 1 ? (return v) : (puts "no")
  :end
end
p tern(1, 7), tern(2, 7)
i = 0
while true
  i += 1
  (break) if i == 3
end
p i
r = [1, 2, 3].each do |e|
  (next) if e == 1
  (break e * 10) if e == 2
end
p r
def val(n)
  x = (return :early if n == 0; n * 2)
  x + 1
end
p val(0), val(3)
