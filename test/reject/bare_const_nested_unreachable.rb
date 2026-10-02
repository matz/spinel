# X is defined only as A::X. CRuby's lookup from the top level never looks
# inside A, so `X` raises NameError (uninitialized constant X); spinel bound
# the leaf name to A::X and printed "A::X".
module A
  class X; def w = "A::X"; end
end
def g = X.new.w
p g
