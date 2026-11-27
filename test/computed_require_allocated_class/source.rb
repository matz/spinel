class DataNotFound < StandardError
end

# initialize always raises, but `new` of the class's own builds the object
# past it: an object is made, and its computed require runs
class RubySource
  def self.new
    o = allocate
    o.instance_variable_set(:@base, ".")
    o
  end

  def initialize
    raise DataNotFound, "the data gem could not be found"
  end

  def load_index(name)
    require_relative File.join(@base, name)
  end
end
