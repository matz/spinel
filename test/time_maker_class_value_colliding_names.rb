# A Time maker on a Class value where two modules nest a class of one name:
# the arms named each class by its Ruby path, whose leaf the colliding names
# had been renamed apart from, so the comparison read no class and the build
# was refused. Two `Parser` modules (one with a one-argument `at`, as fugit's
# beside tzinfo's two-argument `klass.at`) and two Time subclasses `Stamp`.
module Cron
  module Parser
    def self.word(i)
      "word #{i}"
    end
  end
end

module Nat
  module Parser
    def self.at(i)
      "at #{i}"
    end
  end
end

module A
  class Stamp < Time
    def tag
      "a"
    end
  end
end

module B
  class Stamp < Time
    def tag
      "b"
    end
  end
end

def make(klass = Time)
  klass.at(86400, 0)
end

puts make.utc.year
puts make(A::Stamp).tag
puts make(B::Stamp).tag
puts make(B::Stamp).utc.day
begin
  make(Nat::Parser)
rescue ArgumentError => e
  puts e.message
end
puts Nat::Parser.at(1)
puts Cron::Parser.word(2)
