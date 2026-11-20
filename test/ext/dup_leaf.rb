# --ext-class with two classes of one leaf name: a path names one of them,
# and the name alone is refused (ext-test, emission only). A top-level
# class named like the table's key of the path (ExtDupB__Base) is not the
# one the path names. A path from the root (::ExtDupTop) names a top-level
# class, and its table has the rows of an optional parameter and of new.
class ExtDupTop
  def initialize(n = 0)
    @n = n
  end

  def d(x = 4)
    @n + x
  end
end

class ExtDupB__Base
  def self.c
    3
  end
end

module ExtDupA
  class Base
    def self.a
      1
    end
  end
end

module ExtDupB
  class Base
    def self.b
      2
    end
  end
end

module ExtDup
  #: () -> Integer
  def self.go
    ExtDupA::Base.a + ExtDupB::Base.b + ExtDupB__Base.c
  end

  #: () -> Integer
  def self.top
    ExtDupTop.new.d
  end
end

puts ExtDup.go if __FILE__ == $0
