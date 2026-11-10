# `alias go send` names the send that the class reaches: its own, a
# superclass's, or a mixed-in module's. Only when no class or module of the
# program defines send on that path does the alias name Kernel's send.
# The alias was made a call of Kernel's __send__ even when a superclass or
# an included module defined its own send.

class Base; def send(*a) = "base send #{a.inspect}"; end
class Sub < Base
  alias go send
  def x = "x called"
end
p Sub.new.go(:x)

module M; def send(*a) = "M send #{a.inspect}"; end
class Inc
  include M
  alias go2 send
  def y = "y called"
end
p Inc.new.go2(:y)

module N
  def send(*a) = "N send #{a.inspect}"
  alias go3 send
end
class UsesN; include N; def z = "z"; end
p UsesN.new.go3(:z)

class Pre
  prepend M
  alias go5 send
  def v = "v"
end
p Pre.new.go5(:v)

class Deep < Sub
  alias_method :go6, :send
end
p Deep.new.go6(:x)

# no send of the program on the path: Kernel's send
class Plain
  alias go4 send
  def w = "w called"
end
p Plain.new.go4(:w)

class Base2; def public_send(*a) = "base ps #{a.inspect}"; end
class Sub2 < Base2
  alias go7 public_send
  def x = "x"
end
p Sub2.new.go7(:x)
