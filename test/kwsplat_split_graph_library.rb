# A split call whose empty branch needs a library the graph model loads from the parse (builtins/enumerable.rb for a block-only index), nested in itself.
e = {}
r = []
p [[1, 2], [3]].index(**e) { |s| s.index(**e) { |t| r << t; t == 2 }; s == [3] }
p r
