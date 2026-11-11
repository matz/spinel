# respond_to? asked before the program adds the method to Comparable answers false
p "s".respond_to?(:feat3), [1, "s"][1].respond_to?(:feat3)
module Comparable; def feat3 = 1; end
p "s".respond_to?(:feat3), [1, "s"][1].respond_to?(:feat3)
