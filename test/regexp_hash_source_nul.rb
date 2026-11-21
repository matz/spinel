# Regexp#hash covers the whole source: the bytes after a NUL count, so two
# patterns that agree up to a NUL do not hash alike.
a = Regexp.new("a\0b")
b = Regexp.new("a\0c")
p a.source.size
p(a.hash == b.hash)
p(a.hash == Regexp.new("a\0b").hash)
p(a == b)
h = { a => 1 }
h[b] = 2
p h.size
p h[a]
p h[b]
p h[Regexp.new("a\0b")]
