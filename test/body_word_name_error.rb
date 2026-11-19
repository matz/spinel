# A visibility word that names a method the class has no instance method
# of raises NameError where it runs: in a class body, in a block a class
# body runs, in a class method's body (where its value is the method's),
# and for `private def self.m`, which names a class method.
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
