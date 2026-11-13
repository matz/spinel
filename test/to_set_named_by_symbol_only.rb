# A program that names to_set only as a symbol -- never `require "set"`, `Set`
# or a `.to_set` call -- still gets Set, which is core in CRuby 4.0. The source
# scan that splices the set package in looked for `Set` and `.to_set`, so
# respond_to?(:to_set) answered false on an Array or a Hash, and send(:to_set),
# public_send(:to_set) and map(&:to_set) died with a NoMethodError at run time
# (Kilau K-042).
p [1, 2].respond_to?(:to_set)
h = { "a" => 1 }
p h.respond_to?(:to_set)
p (1..3).respond_to?(:to_set)
p [1, 1, 2].send(:to_set).size
p [3, 3].public_send(:to_set).size
p [[1, 1], [2]].map(&:to_set).size
