# A Time maker on a Class value still makes a Time subclass's Time when the
# program reopens Time (a runtime adding a method) or another class has a
# class method of the same name (a parser rule `at`).
class Time
  def stamp_tag
    "stamp"
  end
end

module Rules
  def self.at(i)
    "rule #{i}"
  end
end

class Shifted < Time
  def tag
    "shifted"
  end
end

class Stamp
  def initialize(value)
    @value = value
  end

  def plain
    new_time
  end

  def shifted
    new_time(Shifted)
  end

  def new_time(klass = Time)
    klass.at(@value)
  end
end

def make(klass)
  klass.at(0)
end

puts Stamp.new(0).plain.utc.year
puts Stamp.new(0).shifted.tag
puts Stamp.new(0).shifted.stamp_tag
p make(Rules)
p make(Time).utc.year
p make(Shifted).class
