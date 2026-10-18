# A local written an Integer or, through the rescue clause of a `begin`, a Range. The walk
# that asks whether a written value can be a Range did not read that shape,
# so the local was not taken for one a Range was written into, and `a[r]`
# indexed with it as an Integer: a TypeError where CRuby slices, and
# `b[r] = 99` stored at index 0 where CRuby splices. The shape is read now.
a = [10, 20, 30, 40]
s = "abcdef"
[ARGV.size + 99, ARGV.size].each do |k|
  r = begin
        raise ArgumentError, "boom" unless k == 0
        1
      rescue ArgumentError
        1..2
      end
  p a[r]
  p s[r]
  b = a.dup
  b[r] = 99
  p b
end
