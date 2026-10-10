# A method that fills an empty Hash local with `[]=` and answers it (tally
# in builtins/, or `counts = {}; ...; counts` written by hand) keys it by an
# Integer that can be nil: the local, the method's return and each call of
# it widen to the boxed-key kind, so the nil key is stored and printed
# instead of refused.
def count_keys(a)
  counts = {}
  a.each { |x| counts[x] = counts.fetch(x, 0) + 1 }
  counts
end

def first_seen(a, rev)
  seen = {}
  a.each_with_index { |x, i| seen[x] = i unless seen.key?(x) }
  if rev
    seen
  else
    seen
  end
end

ints = [5, 6]
[ARGV.size + 7, ARGV.size].each do |i|
  b = ints[i]
  p [b, b, 3].tally
  p count_keys([b, 4, b])
  p first_seen([3, b, 3, b], i > 0)
end
