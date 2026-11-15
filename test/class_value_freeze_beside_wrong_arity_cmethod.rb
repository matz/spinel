module Clock
  def self.freeze(seconds)
    [seconds]
  end
end

class Link
  def initialize(path)
    @path = path
  end

  def freeze
    @path.freeze
    super
  end

  def path
    @path
  end
end

class Holder
  def initialize(value)
    @value = value.dup.freeze
  end

  def value
    @value
  end
end

values = [Link.new(+"a"), Link, Clock]
p Holder.new(values[0]).value.path
p Holder.new(values[1]).value.frozen?
