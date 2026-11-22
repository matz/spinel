# spinel: reject-subclass: singleton method on self inside an instance method
# An instance method's def self.x is the object's singleton method, not the class side's: the walk reads the frame as an object's, with no class pending on the class's header, after another class's method body has defined a method. The program is refused as before, not aborted by the record's own check.
class C
  def self.mkc = define_method(:a) { 1 }
end
C.mkc
class Base
  def self.x = :base
end
class D < Base
  def inst
    def self.x = :obj
  end
  def self.hide = private_class_method(:x)
end
p D.x
D.hide
p((D.x rescue :nme_private))
p D.send(:x)
o = D.new
o.inst
p o.x, Base.x
