# `a[i] OP x` on an Integer array: a nil element raises at the operator,
# after the right operand ran, as CRuby reads a[i], runs x, then calls OP.
# A right operand that raises (ZeroDivisionError) raises first.
def show
  p yield
rescue => e
  puts "#{e.class}: #{e.message}"
end
a = [1]
a[3] = 2
j = 7
z = 0
show { a[0] + j % 3 }
show { a[1] + j % 3 }
show { a[1] + j % z }
show { a[0] - j / z }
show { a[1] * (j * 2) }
show { a[5] + j % 3 }
