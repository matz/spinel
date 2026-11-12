# --ext-class with two classes of one leaf name: a path names one of them,
# and the name alone is refused (ext-test, emission only). A top-level
# class named like the table's key of the path (ExtDupB__Base) is not the
# one the path names.
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
end

puts ExtDup.go if __FILE__ == $0
