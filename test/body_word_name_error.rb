# A visibility word that names a method the class has no instance method
# of raises NameError where it runs: in a class body, in a block a class
# body runs, in a class method's body (where its value is the method's),
# and for `private def self.m`, which names a class method. A builtin
# method the class has (Kernel's puts) is no such name, and the message
# names the method as it is spelled.
class K
  begin
    private def self.cs = :cs
  rescue NameError => e
    p e.class
  end
end
p [(K.cs rescue $!.class), K.respond_to?(:cs)]
class K
  begin
    private :nope
  rescue NameError => e
    p [e.class, e.message]
  end
end
pr = proc do
  class K
    begin
      private :nope2
    rescue NameError => e
      p [e.class, e.message]
    end
  end
  nil
end
pr.call
class K
  def self.h
    private :nope3
  rescue NameError => e
    p [e.class, e.message]
  end
end
K.h
# a class body's word on a builtin module's method the class includes
# answers its name, and a module's NameError names the module
class T0
  include Comparable
  x = private(:between?)
  p x
end
module T1
  begin
    private :nope4
  rescue NameError => e
    p e.message
  end
end
class K
  x = private(:puts)
  p x
  begin
    private :"a\"b"
  rescue NameError => e
    p e.message
  end
end
# a class method's word on a name nothing defines leaves no method a
# reader counts, before or after it raises, and an Array's names raise
# too, in a class method and in a class body
class N0
  def self.hide = private(:zz)
  def self.hide2 = private([:zz, :zz2])
end
p [N0.private_method_defined?(:zz), N0.new.respond_to?(:zz, true)]
p [(N0.hide rescue $!.class), (N0.hide2 rescue $!.class)]
p [N0.private_method_defined?(:zz), N0.new.respond_to?(:zz, true)]
class N1
  begin
    private [:zz]
  rescue NameError => e
    p e.message
  end
end
