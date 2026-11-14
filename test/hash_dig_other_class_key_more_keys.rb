# A first key of another class than the keys misses, and with more keys the
# answer is the walk's nil whatever the values are; every key is still taken.
h = { 1 => 2 }
p h.dig("a", "b")
p h.dig(:a, 1)
s = { "x" => "y" }
p s.dig(1, 2)
f = { 1.5 => [1] }
p f.dig("k", 0)
i = 0
p h.dig("a", (i += 1))
p i
p h.dig("a")
p({1 => {2 => 3}}.dig(1, "b"))
