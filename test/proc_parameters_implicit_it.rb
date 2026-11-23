# Proc#parameters of a block that reads `it` names no parameter (`_1` and `_2` are named), in a proc and in a lambda.
p proc { it }.parameters, lambda { it }.parameters, -> { it }.parameters
p proc { _1 }.parameters, lambda { _1 }.parameters, -> { _1 }.parameters
p proc { _1 + _2 }.parameters, -> { _1 + _2 }.parameters
p proc { |x| }.parameters
p proc { it }.arity, lambda { it }.arity, -> { _1 }.arity, proc { _1 }.arity
