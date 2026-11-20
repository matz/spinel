# --ext-class fixture: methods with optional and rest parameters, which a
# host calls by name with each count of arguments they take
# (methods_opt_host.c): a method the named class inherits, a singleton
# method, a method the named class defines again with fixed parameters, a
# method made private after its def, and a method with a keyword
# parameter, which the table leaves out. A module that the named class
# includes, and a module that module includes, are not in the list: their
# methods with an optional parameter have their rows too. So have the
# methods of a module that the top level mixes in with the class as the
# receiver (Extra). Alt::Deep and Deep have one leaf name and Alt::Deep
# includes Deep: the search for the classes that include them ends.
# methods_blk.rb has the methods with a block.
module ExtOpt
  module Deep
    def deep(n = 1)
      "deep #{n}"
    end
  end

  module Alt
    module Deep
      include ExtOpt::Deep

      def alt(n = 1)
        "alt #{n}"
      end
    end
  end

  module Extra
    def extra(n = 5)
      "extra #{n}"
    end
  end

  module Tags
    include Deep

    def tag(t = 7, u = 8)
      "tag #{t} #{u}"
    end
  end

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
    include Tags

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

ExtOpt::Rel.include(ExtOpt::Extra)
