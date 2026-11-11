# --ext-class fixture: a kernel whose objects a C host holds, and whose
# instance methods the host calls by name through the exported method
# table (methods_host.c): a method of the class, one it inherits, one it
# overrides with super, a String parameter and result, a nullable Integer
# result, a method answering an object, the attr readers and a writer, a
# singleton method, and a private method that the table leaves out.
module ExtMethods
  class Counter
    attr_reader :name, :n
    attr_accessor :tag

    def initialize(name)
      @name = name
      @n = 0
      @tag = ""
    end

    def bump(k)
      @n += k
    end

    def label(sep)
      name + sep + n.to_s
    end

    def half
      n.even? ? n / 2 : nil
    end

    def twin
      Counter.new(name + "2")
    end

    # a Hash parameter: a value of another kind is TypeError at the boundary
    def keys(h)
      h.keys.map(&:to_s).sort.join(",")
    end

    # a parameter with a default ahead of a required one: not in the table
    def opt_mid(x = 1, y)
      x + y
    end

    # a Range parameter: a kind the thunk cannot check a host's argument
    # against before it unboxes it, so not in the table
    def span(r)
      r.size
    end

    # nullable parameters: nil reaches the method; a value of another kind
    # is TypeError
    def half_of(n)
      n ? n / 2 : -1
    end

    def greet(s)
      s ? "hi " + s : "hi nobody"
    end

    # an object parameter the method reads: nil is TypeError at the boundary
    def twin_name(c)
      c.name
    end

    # declared to answer a Counter, answers a Loud: the box carries the
    # Loud's class
    def pick(loud)
      loud ? Loud.new(name) : Counter.new(name)
    end

    # a class that only this method makes an instance of
    def spare
      Spare.new
    end

    # an object with to_str: a String parameter takes its to_str, as a
    # builtin's does
    def named
      Named.new
    end

    # a String parameter changed in place: the host's String is the handle
    # the method appends to, so the host sees the change, as in CRuby
    def stamp(s)
      s << "!"
      s
    end

    def self.opts
      { b: 1, a: 2 }
    end

    private def secret
      42
    end

    # a singleton method: called on the class value, inherited by Loud
    def self.zero
      Counter.new("zero")
    end

    # a singleton method over a class instance variable that nothing in the
    # program calls: the host calls it on each class value, so the subclass
    # gets its own copy (its own variable), and the defining class's row
    # still calls this one
    def self.tally
      @tally ||= 0
      @tally += 1
    end

    # a define_method in a method nothing in the program names: the host
    # names it, so the definition stays
    def self.install_dyn
      define_method(:dyn) { name + "!" }
    end

    def self.call_dyn(c)
      c.dyn
    end

    # a method the table leaves out (an optional parameter) that nothing
    # names: its define_method defines nothing, as before the flag, so the
    # to_s it would define does not refuse the compile
    def self.unused(x = 1)
      define_method(:to_s) { "x#{x}" }
    end
  end

  class Loud < Counter
    def shout
      name.upcase
    end

    def bump(k)
      super(k * 2)
    end
  end

  class Spare
    def hi
      "spare"
    end
  end

  class Named
    def to_str
      +"nd"
    end
  end

  # the entries answer boxed objects, as a host holds them
  #: (String) -> untyped
  def self.make(name)
    Counter.new(name)
  end

  #: (String) -> untyped
  def self.loud(name)
    Loud.new(name)
  end
end

if __FILE__ == $0
  c = ExtMethods.make("c")
  c.bump(1)
  puts c.label(":")
  p c.half
  puts c.twin.name
  puts c.keys(ExtMethods::Counter.opts)
  l = ExtMethods.loud("l")
  l.bump(2)
  puts l.shout, l.n
  puts c.pick(true).class, c.spare.hi, c.stamp(+"s")
  puts c.opt_mid(2), c.half_of(4), c.half_of(nil), c.greet("you"), c.greet(nil), c.twin_name(c)
  puts c.span(1..3)
end
