# block-locals `; x` against an outer x, at two levels, in a lambda, paramless, reset each iteration
x = "outer"
[1, 2].each { |i; x| x ||= i * 3; p x }
p x
l = ->(a; x, y) { x = a; y = [x]; [x, y] }
p l.(7), x
[1].each { |; x| x = :inner; [2].each { |j; x| x = j; p x }; p x }
r = []
3.times { |k; acc| acc = (acc || 0) + k; r << acc }
p r, x
