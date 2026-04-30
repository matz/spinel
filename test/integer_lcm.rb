# basic
puts 6.lcm(4)
puts 4.lcm(6)

# zero
puts 0.lcm(5)
puts 5.lcm(0)
puts 0.lcm(0)

# negative
puts((-4).lcm(6))
puts 6.lcm(-4)
puts((-3).lcm(-7))

# same
puts 7.lcm(7)

# one
puts 1.lcm(5)
puts 5.lcm(1)

# coprime
puts 8.lcm(9)

# divisor — one divides the other
puts 3.lcm(9)
puts 9.lcm(3)

# primes
puts 7.lcm(13)

# one with one
puts 1.lcm(1)

# large
puts 12345.lcm(67890)
