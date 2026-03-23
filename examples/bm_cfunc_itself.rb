# cfunc_itself benchmark (from yjit-bench)
# Integer#itself returns self

total = 0
n = 0
while n < 500000
  i = 0
  while i < 10
    total = total + i.itself
    i = i + 1
  end
  n = n + 1
end
puts total
puts "done"
