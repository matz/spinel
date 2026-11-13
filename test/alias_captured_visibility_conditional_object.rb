# A visibility word that may not have run, in a reopened Object, leaves the
# alias of a Kernel method with what its section says: the walk to Kernel's
# private method must not stand for an answer the program may overrule.

if ARGV.size == 0
  class Object
    public :puts
  end
end
class Taken
  alias pp2 puts
end

class Object
  public :p
end
if ARGV.size > 0
  class Object
    private :p
  end
end
class Sure
  alias_method :p2, :p
end

[Taken, Sure].each do |k|
  p [k, k.instance_methods(false).sort, k.private_instance_methods(false).sort]
end
