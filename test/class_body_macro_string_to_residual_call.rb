# A class-body macro hands a String it made to a call of the class's own
# method, left as written where the macro call stands (`keep(fk)`, a
# belongs_to's foreign key). The statement shares the String the macro
# holds: written once into the class body's statements, read by the call
# and by the method define_method makes, one object, as in CRuby. It was
# refused under the graph load model. Where the macro reads the String
# again after the call, which may change it, it stays refused.
module M
  def bt(name)
    fk = "#{name}_id"
    keep(fk)
    define_method(name) { fk }
  end
end
class K
  extend M
  def self.keep(x) = (@kept ||= []) << x
  def self.kept = @kept
  bt :list
  bt :item
end
p K.kept, K.new.list, K.new.list.equal?(K.kept[0]), K.new.item.equal?(K.kept[1])
K.kept[0] << "!"
p K.new.list
module N
  def bt2(name)
    fk = "#{name}_ref"
    mark(fk)
    define_method(name) { fk }
  end
end
class L
  extend N
  def self.mark(x) = x << "!"
  bt2 :a
end
p L.new.a
