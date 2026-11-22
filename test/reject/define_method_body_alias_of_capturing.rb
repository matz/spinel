# A define_method block in a method's body that closes over that method's
# locals, where the program aliases the name it defines, is refused: an alias
# keeps the method it found, which is the closure of one run of the method,
# and the compiled method reads the environment of the run that came last.
class K
  def self.mk(n)
    define_method(:m) { n }
    :ok
  end
end
K.mk(1)
class K
  alias_method :old, :m
end
K.mk(2)
p K.new.old, K.new.m
