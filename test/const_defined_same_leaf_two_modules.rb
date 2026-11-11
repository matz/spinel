# Two modules that each own a constant of the same name: const_defined?
# on each one is true, on a literal receiver and on a class value.
def later = U.const_defined?(:H)
module U; end
p later
module U; H = 1; end
module U2; H = 2; end
p later

module A; end
module B; end
p A.const_defined?(:X), B.const_defined?(:X)
module A; X = 1; end
module B; X = 3; end
module C; end
p A.const_defined?(:X), B.const_defined?("X")
p A.const_defined?(:X, false), C.const_defined?(:X)
p A.const_get(:X), B.const_get(:X)
p [A, B].all? { |m| m.const_defined?(:X) }
p [A, B, C].map { |m| m.const_defined?(:X) }
