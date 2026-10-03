# A receiverless call in a method added to a builtin class is a call on
# self, wherever the method is defined: at the top of the class body, under
# a guard (`def blank? = strip.empty? unless method_defined?(:blank?)`), in
# a begin, or by define_method. A guarded or define_method body was left
# out of the rewrite to `self.name`, and answered from a short inline list:
# bare Float#round and #ceil printed 3.0, Symbol#length raised NameError,
# and center, split, tr and divmod were refused.
class String
  def blank3? = strip.empty? unless method_defined?(:blank3?)
  if true
    def loud = upcase + "!"
  end
  begin
    def twice = self * 2
  end
  define_method(:dm_up) { upcase }
  def in_block = [1, 2].map { |i| length + i }
  def to_sym_twice = to_sym.to_s * 2
end
class Integer
  def dbl = (self + self).to_s unless method_defined?(:dbl)
  define_method(:dm_succ) { succ }
end
class Float
  def fl3 = floor unless method_defined?(:fl3)
end
class Symbol
  def sz = size unless method_defined?(:sz)
end
p " ".blank3?, "a".blank3?
p "hi".loud
p "ab".twice
p "x".dm_up
p "abc".in_block
p "q".to_sym_twice
p 21.dbl
p 4.dm_succ
p 2.75.fl3
p :abc.sz

class Float
  def r4 = round unless method_defined?(:r4)
  def c4 = ceil unless method_defined?(:c4)
  define_method(:t4) { to_i + 1 }
end
class Integer
  def h4 = (self / 2) unless method_defined?(:h4)
  define_method(:p4) { pow(2) }
  define_method(:dm4) { divmod(4) }
end
class String
  define_method(:c4) { center(7, ".") }
  define_method(:sp4) { split("-") }
  def tr4 = tr("a", "b") unless method_defined?(:tr4)
end
class Symbol
  define_method(:l4) { length }
  define_method(:u4) { upcase }
end
p 2.5.r4, 2.1.c4, 3.9.t4
p 7.h4, 7.p4, 7.dm4
p "ab".c4, "a-b".sp4, "banana".tr4
p :abc.l4, :ab.u4
