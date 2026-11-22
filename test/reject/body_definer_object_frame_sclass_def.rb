# spinel: reject-subclass: unsupported call
# A class << self def in an instance method is the object's singleton, never called; the class's x comes from an extended module and a class method's private_class_method acts on it. The program is refused as before, not aborted by the record's own check.
module M
  def x = :m
end
class C
  def self.mkc = define_method(:a) { 1 }
end
C.mkc
class D
  extend M
  def inst
    class << self
      def x = :obj
    end
  end
  def self.hide = private_class_method(:x)
end
p D.x
D.hide
p((D.x rescue :nme_private))
p D.send(:x)
