# spinel: reject-subclass: unsupported call
# As body_definer_object_frame_def_self, with the instance method never called: the def self.x spells a class-side name only in the tree, and the word on the inherited x stays a call the record cannot follow.
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
p D.send(:x), Base.x
