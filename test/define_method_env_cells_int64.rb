# spinel: int64
# A define_method block that closes over Integers that pass 2^31 reads and
# writes them as the method has them (also under --int-overflow=promote).
class Big
  def self.setup(base)
    big = base * 1_000_000
    define_method(:m) { |x = 1| big * x }
    define_method(:grow) { big += 2**33 }
    define_singleton_method(:sm) { big + 2**33 }
  end
end
Big.setup(3_000)
p Big.new.m, Big.new.m(7), Big.sm
b = Big.new
p b.grow, b.m
Big.setup(5_000_000)
p Big.new.m, Big.new.m(2) / 3, Big.sm.class
