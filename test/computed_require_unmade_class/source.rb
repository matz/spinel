class DataNotFound < StandardError
end

class Base
  def initialize
    @cache = {}
  end
end

class RubySource < Base
  def initialize
    super
    begin
      raise LoadError, "cannot load such file -- missing/data"
    rescue LoadError
      raise DataNotFound, "the data gem could not be found"
    end
    @base = "missing/data"
  end

  def load_index(name)
    require File.join(@base, name)
  end
end

def pick_source
  RubySource.new
rescue DataNotFound
  :fallback
end
