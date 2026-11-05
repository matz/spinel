# Numeric#step(by:) without to: is endless, as step(nil, by) is
p 1.step(by: 3).first(2)
p 1.0.step(by: 0.25).first(3)
xs = []
5.step(by: -2) { |i| break if i < 0; xs << i }
p xs
n = 0
r = 2.step(by: (n += 1; 4)) { |i| break i if i > 9 }
p r, n
p 1.step(by: 2).lazy.map { |i| i * i }.first(3)
p 1.step(10, by: 3).to_a
p 1.step(by: 2, to: 7).to_a
