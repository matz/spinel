# ... and with `public :puts` in a reopened Kernel.

module Kernel
  public :puts
end
class K
  alias pp2 puts
  alias_method :pp3, :print
end
p K.instance_methods(false).sort
p K.private_instance_methods(false).sort
