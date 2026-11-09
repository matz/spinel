# spinel: reject-subclass: a def under a condition only the run time decides, of a name another method answers
# A module's def under a run-time condition, of a name another method
# answers (D#m): C reaches the module twice, directly and through N, and
# D's own m stands in front of it. The two includers cannot both be given
# an entry that chooses the version, so the def is refused rather than
# standing as if it ran (CRuby: NoMethodError for C.new.m, :d for D.new.m).
module M
  def m = :m if ENV["SPINEL_TEST_UNSET_VARIABLE"]
end
module N
  include M
end
class C
  include M
  include N
end
class D
  include M
  def m = :d
end
p((C.new.m rescue :nomethod))
p D.new.m
