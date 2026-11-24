# A visibility word in a class whose chain the record cannot follow: a later def of a name the runtime calls back (`to_s`) leaves
# every top-level class opaque to it, and a word in `1.times` is not one it can place. A name a method's body may have made counts as
# the method's extent says there, as it did before words tested the cells: the word applies to it where the method surely ran, and
# `zz` stays the method it is (it is not answered present where it was never defined).
YES = !ENV["SPINEL_NO_SUCH_VARIABLE_FOR_THIS_TEST"]
puts "opaque to_s, run true"
class F106
  def self.mk106(f)
    F106.class_eval { def zz = :zz } if f
  end
  def tail = :tail

end
F106.mk106(true)
class F106
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end
class Pt106; def to_s = 'pt'; end; puts Pt106.new
c106 = F106.new
p [c106.respond_to?(:zz), (c106.zz rescue $!.class), (c106.tail rescue $!.class), F106.private_instance_methods(false).sort, F106.public_method_defined?(:tail)]

puts "word in 1.times, run true"
class F107
  def self.mk107(f)
    F107.class_eval { def zz = :zz } if f
  end
  def tail = :tail

end
F107.mk107(true)
class F107
  begin; 1.times { private :zz, :tail }; rescue NameError => e; p e.message; end
end

c107 = F107.new
p [c107.respond_to?(:zz), (c107.zz rescue $!.class), (c107.tail rescue $!.class), F107.private_instance_methods(false).sort, F107.public_method_defined?(:tail)]
