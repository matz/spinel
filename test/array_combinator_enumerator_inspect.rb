p [[1, 2].cycle(2).inspect, [1, 2, 3].combination(2).inspect]
p [1, 2].cycle(0).inspect
p [1, 2].cycle.inspect
p [1, 2].permutation.inspect
p [1, 2].permutation(1).inspect
p [1, 2].repeated_combination(2).inspect
p [1, 2].repeated_permutation(2).inspect
p ["a", "b"].combination(1).inspect
p [1.0, 2.0].combination(1).inspect
p [1, 2].cycle(2).to_a
p [1, 2, 3].combination(2).to_a
e = [1, 2].cycle(2)
seen = []
result = e.with_index { |value, index| seen << [value, index] }
p seen
e = [1, 2, 3].combination(2)
seen = []
result = e.with_index { |value, index| seen << [value, index] }
p seen
# A walk over a stored combinator answers its receiver.
p [1, 2, 3].permutation(2).with_index { }.size
