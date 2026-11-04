# A Time maker on a Class value that no class method of its name can take:
# a class whose maker takes another shape raises ArgumentError, as CRuby
# does, and any other class value NoMethodError.
module One
  def self.at(i)
    "one #{i}"
  end
end

module Need
  def self.at(i, unit:)
    "need #{i} #{unit}"
  end
end

def make(klass)
  klass.at(1, 2)
rescue ArgumentError, NoMethodError => e
  "#{e.class}: #{e.message}"
end

p make(One)
p make(Need)
p make(Comparable)
p make(Time).utc.year
