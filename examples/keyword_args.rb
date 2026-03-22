# Keyword arguments benchmark (from yjit-bench)

def add(left:, right:)
  left + right
end

total = 0
n = 0
while n < 500000
  total = total + add(left: 1, right: 0)
  total = total + add(left: 1, right: 1)
  total = total + add(left: 1, right: 2)
  total = total + add(left: 1, right: 3)
  total = total + add(left: 1, right: 4)
  total = total + add(left: 1, right: 5)
  total = total + add(left: 1, right: 6)
  total = total + add(left: 1, right: 7)
  total = total + add(left: 1, right: 8)
  total = total + add(left: 1, right: 9)
  n = n + 1
end
puts total
puts "done"
