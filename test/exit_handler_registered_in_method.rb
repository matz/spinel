# A handler a method registers during the load runs after the load, and what
# it calls is not run where the method registered it. A Proc only a handler
# calls, a method only handlers call and a class method a handler defines
# make their versions then.
class K
  def self.reg
    at_exit { K.setup; puts "reg #{K.new.m}" }
  end
  def self.setup
    def m = :k
  end
end
puts (K.new.m rescue $!.class)
K.reg
puts (K.new.m rescue $!.class)
puts K.new.respond_to?(:m)

class P; def baz = "baz"; end
def readp(tag) = puts("#{tag} #{P.new.baz} #{P.method_defined?(:nw)} #{(P.new.nw rescue $!.class)}")
pr = proc do
  class P
    def baz = "new"
    def nw = "nw"
  end
  nil
end
at_exit { readp(:proc_exit) }
at_exit { pr.call }
readp(:proc_load)

class Q; end
def readq(tag) = puts("#{tag} #{(Q.cm rescue $!.class)} #{Q.respond_to?(:cm)}")
at_exit { readq(:cm_exit) }
at_exit do
  class Q
    def self.cm = :cm
  end
  nil
end
readq(:cm_load)
