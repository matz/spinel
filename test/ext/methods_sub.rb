# --ext-class with one class named, a subclass: the host reaches the
# methods its ancestor defines through it (methods_sub_host.c), and a
# define_method in such a method, which nothing in the program names,
# stays registered.
module ExtSub
  class Base
    def self.install
      define_method(:extra) { "installed" }
    end

    def self.call_extra(o)
      o.extra
    end

    def base_name
      "base"
    end
  end

  class Sub < Base
    def hi
      "hi"
    end
  end

  #: () -> untyped
  def self.make
    Sub.new
  end
end

if __FILE__ == $0
  s = ExtSub.make
  puts s.hi, s.base_name
end
