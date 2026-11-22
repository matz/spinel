# A define_method block in a method's body that yields to that method's
# block is refused where the program calls the name it defines: the method it
# defines has no block of the method that defined it.
class K
  def self.mk
    define_method(:m) { yield }
    :ok
  end
end
K.mk { 5 }
p K.new.m
