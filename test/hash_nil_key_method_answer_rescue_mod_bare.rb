# A method that fills an empty Hash local with `[]=`, keyed by an Integer that
# can be nil, and answers it as the rescue expression of a rescue modifier
# (`seen.dup rescue seen`), with no parentheses to end a list on the read. The
# search for the reads that answer the method looked at the last statement of
# each list only, so the read was not the answer and the Hash stayed typed: the
# nil key was refused at run time (a TypeError) where CRuby stores it. Every
# value position of the body that reads the local answers now.
def first_seen(a, k)
  seen = {}
  a.each_with_index { |x, i| seen[x] = i unless seen.key?(x) }
  seen.dup rescue seen
end
ints = [5, 6]
[ARGV.size + 7, ARGV.size].each do |i|
  b = ints[i]
  p first_seen([3, b, 3, b], i + 1)
end
