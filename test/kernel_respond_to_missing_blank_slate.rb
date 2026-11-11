# a class under BasicObject without Kernel has no Kernel#respond_to_missing? to answer, receiverless or through a subclass
class Blank < BasicObject
  def t = respond_to_missing?(:x, false)
end
class BlankSub < Blank; def respond_to_missing?(n, a = false) = true; end
begin; p Blank.new.t; rescue ::NoMethodError => e; p e.class; end
begin; p BlankSub.new.t; rescue ::NoMethodError => e; p e.class; end
