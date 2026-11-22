# spinel: reject-subclass: no class in the program defines a class method 'nm'
# A module_function in a method body names a method an include gives the module, the include is recorded after another class's
# body has read the name, and the module_function's reach stops short of it until then. The program is refused as before, not
# aborted by the record's own check: the walk keeps the word barred while an include it has not recorded may give the name a method.
class C
  def self.mkc = define_method(:a) { 1 }
end
C.mkc
module N
  def nm = :n
end
module M
  def self.setup
    module_function :nm
    nil
  end
end
class D
  def self.mk
    attr_accessor(:nm)
    nil
  end
end
D.mk
module M
  include N
end
M.setup
p M.nm
