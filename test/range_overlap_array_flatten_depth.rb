# spinel: gc-stress
# Range#overlap? on two numeric ranges, plus Array#flatten(n)
# with a depth-bounded variant.

puts (1..5).overlap?(4..10)
puts (1..3).overlap?(5..10)
puts (5..10).overlap?(1..3)
puts (1..10).overlap?(20..30)

# Depth-bounded flatten.
puts [[1, [2, [3, [4]]]]].flatten(1).inspect
puts [[1, [2, [3, [4]]]]].flatten(2).inspect
puts [[1, [2, [3, [4]]]]].flatten(3).inspect
# Unbounded still works (no arg).
puts [[1, [2, [3]]]].flatten.inspect

# Depth-bounded flatten unboxes float and string sub-arrays one level too
# (the bounded walker handles IntArray / StrArray / FloatArray / PolyArray).
puts [[1.5, 2.5], [3.5]].flatten(1).inspect
puts [["a", "b"], ["c"]].flatten(1).inspect
