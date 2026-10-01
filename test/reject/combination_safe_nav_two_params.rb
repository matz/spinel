# CRuby spreads each tuple of combination, permutation and their repeated
# forms across a block taking two or more parameters (m is 3, n is 1). Over
# a `&.` call the block keeps its parameters as written, and the emitters
# bind the first parameter only (m the whole tuple, n nil); the block is
# refused at this line instead.
[3, 1]&.combination(2) { |m, n| p [m, n] }
