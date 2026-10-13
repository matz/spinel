# A constant set through its module's path (`Box::TARGET = ...`) holds a
# String, so `def TARGET.m` in the module is refused like `NAME = "x"` is
# (test/reject/singleton_untraced_no_self.rb): it was taken for a class
# method, and the call answered String's own upcase ("ABC", CRuby "zzz").
module Box; end
Box::TARGET = "abc" * 1
module Box
  def TARGET.upcase = "zzz"
  p TARGET.upcase
end
