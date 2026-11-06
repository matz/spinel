# A Time maker on the Class a call answers: the call's value is read once
# and dispatched as a Class value in a local is. It was refused (`at` was
# read as Array#at, `undefined method '[]' for a Class`), and self.class
# inside instance_exec or under a `class` override made the wrong class.
class Foo
  def klass
    Time
  end

  def again(n)
    klass.at(n)
  end
end
p Foo.new.again(7).to_i

class Stamp < Time
  def made_by(other)
    other.instance_exec { self.class.at(0) }
  end
end

class Tick < Time
end
p Stamp.at(1).made_by(Tick.at(5)).class

class Clock < Time
  def class
    Time
  end

  def again
    self.class.at(to_i)
  end
end
c = Clock.at(7).again
p c.instance_of?(Time)
p c.to_i
