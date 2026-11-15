# A blockless each_line on a boxed String is an Enumerator with no size:
# `.size` is nil, as CRuby gives, not the count of lines.
x = ["ab\ncd\n", 5].first
p x.each_line.size
p x.each_line.size.nil?
p x.each_line.count
p x.each_line.to_a.size
p x.each_line.map(&:size)
e = x.each_line
p e.size
p x.each_char.size
p x.each_byte.size
h = { k: "a\nb", n: 1 }
p h[:k].each_line.size
def lines_of(v) = v.each_line.size
p lines_of(x)
s = "ab\ncd\n"
p s.each_line.size
