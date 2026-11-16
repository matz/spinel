# --ext-class fixture: methods with optional and rest parameters, which a
# host calls by name with each count of arguments they take
# (methods_opt_host.c): a method the named class inherits, a singleton
# method, a method the named class defines again with fixed parameters, a
# method made private after its def, and a method with a keyword
# parameter, which the table leaves out. methods_blk.rb has the methods
# with a block.
module ExtOpt
  class Base
    def initialize
      @log = []
    end

    def where(cond = nil, *args)
      @log << "where(#{cond.inspect}, #{args.size})"
      self
    end

    def self.pick(a, b = 2)
      a + b
    end

    # the named class defines it with a fixed parameter: its row is that one
    def lim(n = 1)
      @log << "base lim #{n}"
      self
    end

    def hid(x = 1)
      x
    end
    private :hid

    def show
      @log.join(" ")
    end
  end

  class Rel < Base
    def order(*args)
      @log << "order(#{args.size})"
      self
    end

    def lim(n)
      @log << "lim #{n}"
      self
    end

    def kw(a = 1, k: 2)
      a + k
    end
  end

  #: () -> untyped
  def self.make
    Rel.new
  end
end
