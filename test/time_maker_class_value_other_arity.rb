# A Time maker on a Class value is still Time's or a Time subclass's when
# another class has a class method of the same name taking another count
# (fugit's parser rule `at(i)` beside tzinfo's `klass.at(value, usec)`):
# that method cannot answer the call, which it would only raise
# ArgumentError in.
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

  def new_time(klass = Time)
    klass.at(@value, 5 * 1000000)
  end
end

p Rules.at(1)
p Stamp.new(0).new_time.utc.year
p Stamp.new(0).new_time(Shifted).tag
p Stamp.new(0).new_time(Shifted).usec
