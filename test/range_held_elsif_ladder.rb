# A local written an Integer in the first arms of an `elsif` ladder of 40
# arms and a Range in the last. The walk that asks whether a written value can
# be a Range gave up after 32 steps of the ladder, so the local was not taken
# for one a Range was written into, and `a[r]` indexed with it as an Integer:
# a TypeError where CRuby slices, and `b[r] = 99` stored at index 0 where
# CRuby splices. The ladder is read to its end.
a = [10, 20, 30, 40]
s = "abcdef"
[ARGV.size + 99, ARGV.size + 1].each do |k|
  r = if k == 1 then 1
    elsif k == 2 then 2
    elsif k == 3 then 0
    elsif k == 4 then 1
    elsif k == 5 then 2
    elsif k == 6 then 0
    elsif k == 7 then 1
    elsif k == 8 then 2
    elsif k == 9 then 0
    elsif k == 10 then 1
    elsif k == 11 then 2
    elsif k == 12 then 0
    elsif k == 13 then 1
    elsif k == 14 then 2
    elsif k == 15 then 0
    elsif k == 16 then 1
    elsif k == 17 then 2
    elsif k == 18 then 0
    elsif k == 19 then 1
    elsif k == 20 then 2
    elsif k == 21 then 0
    elsif k == 22 then 1
    elsif k == 23 then 2
    elsif k == 24 then 0
    elsif k == 25 then 1
    elsif k == 26 then 2
    elsif k == 27 then 0
    elsif k == 28 then 1
    elsif k == 29 then 2
    elsif k == 30 then 0
    elsif k == 31 then 1
    elsif k == 32 then 2
    elsif k == 33 then 0
    elsif k == 34 then 1
    elsif k == 35 then 2
    elsif k == 36 then 0
    elsif k == 37 then 1
    elsif k == 38 then 2
    elsif k == 39 then 0
    elsif k == 40 then 1
      else 1..2
      end
  p a[r]
  p s[r]
  b = a.dup
  b[r] = 99
  p b
end
