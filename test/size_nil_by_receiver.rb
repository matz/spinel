# Whether `size` can answer nil depends on the receiver, not on the name:
# a String, an Array or a Hash always has a size, an Enumerator whose size is
# unknown (Enumerator.new without one, an unsized lazy chain) answers nil, and
# so does a non-numeric Range. A method or a slot that sees both kinds answers
# each one's own: nil where the receiver at run time is an unsized one, the
# Integer where it is a sized one, also when the Integer is computed on.
n = ARGV.size
p [1, 2, 3].each_slice(2).size
p Enumerator.new { |y| y << 1 }.size
p Enumerator.new(3) { |y| y << 1 }.size
p (1..3).lazy.map { |x| x }.size
p (1..3).lazy.select { |x| x }.size
p ("a".."c").size

def sz(o) = o.size
p sz([1, 2])
p sz(Enumerator.new { |y| y << 1 })
p sz({ a: 1 })
p sz("abc")

x = [[1, 2], Enumerator.new { |y| y << 1 }][n + 1]
p x.size
y = [[1, 2], Enumerator.new { |y| y << 1 }][n]
p y.size
s = [3, 4].each_slice(1)
p s.size + 1
