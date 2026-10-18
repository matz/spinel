# A method that fills an empty Hash local with `[]=`, keyed by an Integer that
# can be nil, and answers it through an `elsif` ladder of 34 arms. The search for
# the arm that answers the local gave up at a depth of 32, and an arm of the
# ladder cost a level, so the answer of the last arm was taken for a use that
# keeps the Hash typed: the nil key was refused at run time (a TypeError)
# where CRuby stores it. The ladder is read to its end, and the local, the
# method's return and each call of it widen to the boxed-key kind.
def first_seen(a, k)
  seen = {}
  a.each_with_index { |x, i| seen[x] = i unless seen.key?(x) }
  if k == 1 then seen
  elsif k == 2 then seen
  elsif k == 3 then seen
  elsif k == 4 then seen
  elsif k == 5 then seen
  elsif k == 6 then seen
  elsif k == 7 then seen
  elsif k == 8 then seen
  elsif k == 9 then seen
  elsif k == 10 then seen
  elsif k == 11 then seen
  elsif k == 12 then seen
  elsif k == 13 then seen
  elsif k == 14 then seen
  elsif k == 15 then seen
  elsif k == 16 then seen
  elsif k == 17 then seen
  elsif k == 18 then seen
  elsif k == 19 then seen
  elsif k == 20 then seen
  elsif k == 21 then seen
  elsif k == 22 then seen
  elsif k == 23 then seen
  elsif k == 24 then seen
  elsif k == 25 then seen
  elsif k == 26 then seen
  elsif k == 27 then seen
  elsif k == 28 then seen
  elsif k == 29 then seen
  elsif k == 30 then seen
  elsif k == 31 then seen
  elsif k == 32 then seen
  elsif k == 33 then seen
  elsif k == 34 then seen
  else seen
  end
end

ints = [5, 6]
[ARGV.size + 7, ARGV.size].each do |i|
  b = ints[i]
  p first_seen([3, b, 3, b], 99)
  p first_seen([3, b, 3, b], 1)
end
