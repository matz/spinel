# A method that fills an empty Hash local with `[]=`, keyed by an Integer that
# can be nil, and answers it through a `case`/`in`. The search for the arm
# that answers the local did not read that shape, so the read was taken for
# a use that keeps the Hash typed: the nil key was refused at run time (a
# TypeError) where CRuby stores it. The shape is read now, and the local, the
# method's return and each call of it widen to the boxed-key kind.
def first_seen(a, k)
  seen = {}
  a.each_with_index { |x, i| seen[x] = i unless seen.key?(x) }
  case k
  in 0 then seen
  else seen
  end
end

ints = [5, 6]
[ARGV.size + 7, ARGV.size].each do |i|
  b = ints[i]
  p first_seen([3, b, 3, b], i)
end
