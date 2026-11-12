# A visibility word on Object (or on Kernel) for a Kernel method changes what
# an alias of it takes: with `public :puts` on Object, an alias of puts in a
# class is public.

class Object
  public :puts
end
class K
  alias pp2 puts
  alias_method :pp3, :print
end
p K.instance_methods(false).sort
p K.private_instance_methods(false).sort
