# A Proc or a lambda passed to at_exit with & is registered and runs with the other handlers and
# END, last registered first; the defs its class body makes apply when it runs.
class H
  def baz = "baz"
end
def chk(tag) = puts("#{tag} #{(H.new.baz rescue $!.class)} #{H.new.respond_to?(:baz)} #{H.method_defined?(:nw)} #{(H.new.nw rescue $!.class)}")
pr = proc { puts "ran" }
at_exit(&pr)
l = -> { puts "lam" }
END { puts "end" }
at_exit(&l)
grow = proc do
  class H
    def baz = "new"
    def nw = "nw"
  end
  nil
end
at_exit { chk(:exit) }
p at_exit(&grow).equal?(grow)
chk(:load)
puts "load"

# A handler registered after it runs before it: the class body's defs are not there yet.
class H2
  def baz = "baz"
end
def chk2(tag) = puts("#{tag} #{(H2.new.baz rescue $!.class)} #{H2.method_defined?(:nw)} #{(H2.new.nw rescue $!.class)}")
grow2 = proc do
  class H2
    def baz = "new"
    def nw = "nw"
  end
  nil
end
at_exit(&grow2)
at_exit { chk2(:before_grow) }
chk2(:load2)
