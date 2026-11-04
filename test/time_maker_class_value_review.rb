# A Time maker on a Class value, beside the other class methods of its name:
# a Time subclass's own `at` answers for it, and a candidate taking a
# keyword counts by its positionals alone.
class Own < Time
  def self.at(*args)
    "own #{args.size}"
  end
end

class Plain < Time
  def tag
    "plain"
  end
end

module Kw
  def self.at(i, unit: 1)
    "kw #{i} #{unit}"
  end
end

def make1(klass)
  klass.at(1)
end

def make2(klass)
  klass.at(0, 5)
end

p make2(Own)
p make2(Plain).class
p make2(Time).utc.year
p make1(Kw)
p make1(Time).utc.year


