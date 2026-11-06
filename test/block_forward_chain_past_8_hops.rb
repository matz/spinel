# Ten forwards of a &block in a chain, the one kept at its end, defined
# from the outside in: clearing the forwarders took a round each.
class Hop0
  def add(&block)
    Hop1.new.add(&block)
  end
end

class Hop1
  def add(&block)
    Hop2.new.add(&block)
  end
end

class Hop2
  def add(&block)
    Hop3.new.add(&block)
  end
end

class Hop3
  def add(&block)
    Hop4.new.add(&block)
  end
end

class Hop4
  def add(&block)
    Hop5.new.add(&block)
  end
end

class Hop5
  def add(&block)
    Hop6.new.add(&block)
  end
end

class Hop6
  def add(&block)
    Hop7.new.add(&block)
  end
end

class Hop7
  def add(&block)
    Hop8.new.add(&block)
  end
end

class Hop8
  def add(&block)
    Hop9.new.add(&block)
  end
end

class Hop9
  def add(&block)
    Item.new(&block)
  end
end

class Item
  def initialize(&block)
    @block = block
  end

  def run(x)
    @block.call(x)
  end
end

class Front
  def self.add(&block)
    @last = block
    Hop0.new.add(&block)
  end
end

p Front.add { |x| x * 2 }.run(21)
