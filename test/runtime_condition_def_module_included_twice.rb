# A module's def under a run-time condition, of a name another method
# answers (Z#m, D#m). C includes the module twice; the second include is a
# no-op, as in CRuby, so C has one copy of M in its chain and both includers
# get an entry that chooses the version. With the variable unset the branch
# does not run: C.new.m raises NoMethodError and D's own m answers.
class Z
  def m = :z
end
module M
  def m = :m if ENV["SPINEL_TEST_UNSET_VARIABLE"]
end
class C
  include M
  include M
end
class D
  include M
  def m = :d
end
p((C.new.m rescue :nomethod))
p D.new.m
