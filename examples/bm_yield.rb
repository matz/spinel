# Test program for yield/block support

# User-defined iterator with yield
def my_times(n)
  i = 0
  while i < n
    yield i
    i += 1
  end
end

# Array#each
def sum_array(arr)
  total = 0
  arr.each do |x|
    total += x
  end
  total
end

# Array#map
def double_array(arr)
  arr.map do |x|
    x * 2
  end
end

# Array#select
def evens(arr)
  arr.select do |x|
    x % 2 == 0
  end
end

# Nested yield
def repeat(n)
  i = 0
  while i < n
    yield i
    i += 1
  end
end

# Main
total = 0
my_times(10) do |i|
  total += i
end
puts total  # 45

arr = (1..10).to_a
puts sum_array(arr)  # 55

doubled = double_array(arr)
puts doubled.length  # 10

even_nums = evens(arr)
puts even_nums.length  # 5

# Nested: repeat yields, inner block accumulates
result = 0
repeat(5) do |i|
  my_times(i) do |j|
    result += j
  end
end
puts result  # 0+0+1+0+1+2+0+1+2+3 = 10
