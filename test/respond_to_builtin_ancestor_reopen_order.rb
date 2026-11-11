# respond_to? asked before the program adds the method to Comparable answers false
p "s".respond_to?(:feat3), [1, "s"][1].respond_to?(:feat3)
module Comparable; def feat3 = 1; end
p "s".respond_to?(:feat3), [1, "s"][1].respond_to?(:feat3)
# a call in a method defined after the def answers true;
# a `private :m` in String hides a Comparable method from a boxed String too
module Comparable; def feat4 = 1; end
def chk_after = ["s".respond_to?(:feat4), [1, "s"][1].respond_to?(:feat4)]
p chk_after
module Comparable; def feat5 = 1; end
class String; private :feat5; end
p "s".respond_to?(:feat5), [1, "s"][1].respond_to?(:feat5)
