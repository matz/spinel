# casecmp folds 'A'..'Z' only and compares eight bytes at a time: the
# bytes next to the letters ('@', '[', '`', '{'), a difference in each
# position of a chunk and past it, and different lengths answer as before.
base = "abcdefghIJKLMNOPqrstuvwxYZ@[`{01"
subs = ["@", "[", "`", "{", "a", "A", "z", "Z", "0", "~", "\x7f", " "]
out = []
[0, 1, 7, 8, 9, 15, 16, 17, 31, 32].each do |len|
  a = base[0, len]
  out << a.casecmp(a.swapcase) << a.casecmp(a.upcase) << a.casecmp(a + "x") << (a + "x").casecmp(a)
  len.times do |i|
    subs.each do |ch|
      b = a.dup
      b[i] = ch
      out << a.casecmp(b) << b.casecmp(a.upcase) << (a.casecmp?(b) ? 2 : 3)
    end
  end
end
p out.size, out.sum, out.count(-1), out.count(1), out.count(0)
p out.each_slice(97).map { |s| s.each_with_index.sum { |v, i| v * (i + 1) } }
