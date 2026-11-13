# An alias of a method a builtin class or module above the program's class
# defines takes that method's visibility: Hash's, Array's, Struct's and
# StandardError's own (public) `select`, `[]=`, `sum` and `message`, and
# Enumerable's, are no Kernel#select (private); a def a reopened Kernel makes
# is public in the class that aliases it.

class H < Hash
  alias sel select
  alias st2 []=
end
p H.instance_methods(false).sort
p H.private_instance_methods(false).sort

class Ar < Array
  alias sel select
  alias_method :sm, :sum
end
p Ar.instance_methods(false).sort
p Ar.private_instance_methods(false).sort

St = Struct.new(:a)
class St2 < St
  alias sel select
  alias_method :to2, :to_a
end
p St2.instance_methods(false).sort
p St2.private_instance_methods(false).sort

class Er < StandardError
  alias msg message
  alias_method :bt, :backtrace
end
p Er.instance_methods(false).sort
p Er.private_instance_methods(false).sort

class En
  include Enumerable
  def each; yield 1; end
  alias sel select
  alias_method :fil, :filter
  alias pp2 puts
end
p En.instance_methods(false).sort
p En.private_instance_methods(false).sort

module Kernel
  def zork = 1
end
class Kz
  alias z2 zork
  alias pp2 puts
end
p Kz.instance_methods(false).sort
p Kz.private_instance_methods(false).sort

# a top-level def of the name stands ahead of a reopened Kernel's: Object's
# private one is what the alias takes
def zork2 = 0
module Kernel
  def zork2 = 1
end
class Kz2
  alias z2 zork2
end
p Kz2.instance_methods(false).sort
p Kz2.private_instance_methods(false).sort
