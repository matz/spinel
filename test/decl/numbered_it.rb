# _1/_2/it across sibling blocks and lambdas, types per block, an outer `it` and `_1`-like locals
p [1, 2].map { _1 * 2 }, ["a"].map { _1 + "b" }, [[1, 2]].map { _1 + _2 }
p [3].map { it + 1 }, [:s].map { it.to_s }
l1 = -> { it * 3 }; l2 = -> { _1.to_s * 2 }
p l1.(2), l2.(4)
it_count = 0
[1, 2].each { it_count += it }
p it_count
it = "a local"
p [1].map { it }
p [9].map { |v| [v].map { _1 + v } }
