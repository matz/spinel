# Array#fetch answers its default argument, or its block's value, for an
# index out of range: a default or a block value that can be nil makes the
# answer one that can be nil, held with its nil. In-range reads and non-nil
# defaults answer as before.

a = [1, 2, 3]
n = ARGV.size
p a.fetch(8) { |i| i > 5 ? nil : i }
p a.fetch(1) { |i| i > 5 ? nil : i }
p a.fetch(-9) { |i| i < 0 ? nil : i }
x = a.fetch(4, [3][n + 2])
p x
y = a.fetch(0, [3][n + 2])
p y
z = a.fetch(9) { |i| nil }
p z
w = a.fetch(9, 7)
p w + 1
v = a.fetch(2) { |i| i * 10 }
p v + 1
u = a.fetch(5) { |i| i * 10 }
p u + 1
begin
  a.fetch(5)
rescue IndexError => e
  puts e.message
end
f = [1.5, 2.5]
p f.fetch(3, [0.5][n + 2]), f.fetch(0, nil)
