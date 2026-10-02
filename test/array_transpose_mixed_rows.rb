# Different row representations must preserve every element in each column.
p [[1, 2, 3], ["a", "b", "c"]].transpose
p [["a", "b"], [1, 2]].transpose
p [[1.5, 2.5], [1, 2]].transpose
p [[1, 2], [true, nil]].transpose
p [[:a, :b], [1, 2]].transpose
p [[:a, :b], [:c, :d]].transpose
row = [0, 1, 2]
row.shift
p [row, ["a", "b"]].transpose
p [row, [3, 4]].transpose
