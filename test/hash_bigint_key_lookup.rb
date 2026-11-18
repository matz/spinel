# spinel: int64
# An Integer-keyed Hash looked up with a key typed Bignum
# A Bignum slot can hold a small Integer (2**64 - 2**64 + 5). Such a key was
# decided a miss at compile time, so h[five] answered nil where the entry
# for 5 is.
big = 2**64
five = big - big + 5
h = {5 => "five", 1 => "one"}
g = {5 => 50, 1 => 10}

p h[five], g[five], h[big], g[big]
p h.key?(five), h.include?(five), h.member?(five), h.has_key?(five), h.key?(big)
p h.fetch(five), g.fetch(five), h.fetch(five, "none"), g.fetch(big, 0)
p h.dig(five), g.dig(five), h.dig(big)
p h.values_at(five, 1, big), g.values_at(five, big)
p h.fetch_values(five, 1), g.fetch_values(five)
p h.slice(five, big, 1), g.slice(big)
p h.except(big, five), g.except(five)
p h.assoc(five), g.assoc(big)
p h.to_proc.call(five)

begin
  h.fetch(big)
rescue KeyError => e
  p e.message, e.key
end
begin
  g.fetch_values(1, big)
rescue KeyError => e
  p e.message, e.key
end

d = Hash.new("dflt")
d[5] = "five"
p d[five], d[big]
n = {-3 => "neg"}
p n[big - big - 3], n.key?(big - big - 3)

# a parameter one caller gives a Bignum and another an Integer
def look(h, k) = h[k]
p look(h, five), look(h, 1), look(h, big)

# a boxed key that holds a Bignum
def mixed(f) = f ? 2**64 - 2**64 + 5 : "s"
p h[mixed(true)], h[mixed(false)], h.key?(mixed(true)), g.fetch(mixed(true), 0)
p g.values_at(mixed(true)), g.slice(mixed(true)), g.assoc(mixed(true))

# a boxed key read from an Array of Bignums
a = [big, five]
p h[a[1]], h[a[0]], h.key?(a[0]), h.fetch(a[1])
p g.values_at(a[1], a[0], mixed(false)), h.fetch_values(a[1])
begin
  h.fetch_values(a[0])
rescue KeyError => e
  p e.message, e.key
end

p g.delete(big), g.delete(five), g
p h.delete(mixed(true)), h
