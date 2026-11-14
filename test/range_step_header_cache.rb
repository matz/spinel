# An Integer Range's step(k) { } walks its span in a plain C `for`, as
# upto and times do, but it was not one of the loops that keep their typed
# arrays' headers out of the loop: each `a[i] = v` in it read the array's
# header again, and a loop around it (an upto whose body steps) lost its
# cache too. It is one now; what the loop does to its arrays is unchanged.

# the sieve: upto around a step, a read in one and a store in the other
def sieve(limit)
  primes = Array.new(limit + 1, 1)
  primes[0] = primes[1] = 0
  2.upto(Math.sqrt(limit).to_i) do |p|
    if primes[p] == 1
      (p * p..limit).step(p) { |m| primes[m] = 0 }
    end
  end
  primes.count(1)
end
p sieve(100), sieve(1000)

# a Float array, an exclusive range, a downward step, no argument
def strides
  f = Array.new(10, 0.0)
  (0...10).step(3) { |i| f[i] += 1.5 }
  (9..0).step(-4) { |i| f[i] -= 0.25 }
  (2..4).step { |i| f[i] *= 2.0 }
  f
end
p strides

# a store past the end grows the array; a push in the loop moves it
def grows
  a = [0, 0]
  (0..6).step(2) { |i| a[i] = i }
  b = [1]
  (1..4).step(1) { |i| b << i * 10; b[0] += b[i] }
  [a, b]
end
p grows

# a local the loop reassigns is read as it is, not through a cached header
def swaps
  a = [1, 2, 3, 4]
  other = [10, 20, 30, 40]
  sum = 0
  (0..3).step(1) { |i| sum += a[i]; a = other if i == 1 }
  sum
end
p swaps

# a frozen array refuses the store inside the loop
def frozen
  a = [1, 2, 3].freeze
  (0..2).step(2) { |i| a[i] = 0 }
rescue FrozenError => e
  e.class
end
p frozen

# an endless range left by break; a Float step walks Floats as before
def endless
  a = Array.new(8, 0)
  (1..).step(3) { |i| break if i >= 8; a[i] = i }
  a
end
p endless
g = []
(0..3).step(1.5) { |x| g << x }
p g
