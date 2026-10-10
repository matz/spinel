# A loop's cached store into an Integer or Float array that holds a nil (a
# nil bitmap) clears that element's nil: the store goes through the setter
# while the array has a bitmap, and the element reads back as the value.

def fill(a)
  i = 0
  while i < a.size
    a[i] = i * 10
    i += 1
  end
  a
end

def ffill(a)
  i = 0
  while i < a.size
    a[i] = i * 0.5
    i += 1
  end
  a
end

def bump(a)
  i = 0
  while i < a.size
    a[i] = (a[i] || 0) + 1
    i += 1
  end
  a
end

x = [1, 2, 3]
x[1] = [5][ARGV.size + 3]
p x
p fill(x)
f = [1.5, 2.5]
f[0] = [0.5][ARGV.size + 3]
p ffill(f)
y = [4, 5, 6]
y[2] = [5][ARGV.size + 3]
p bump(y)
