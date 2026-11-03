# A singleton definition on a receiver that cannot be traced to one `new` of
# a user class (a local written twice, an instance variable) defines its
# method on whichever object that is at run time, never on main: the method
# is not callable at the top level and respond_to? there is false. Here none
# of the methods runs.

class Foo; end
module Tag; def tagged = :tagged; end

# define_singleton_method on a local written twice
def dsm_local(flag)
  o = Foo.new
  o = Foo.new if flag
  o.define_singleton_method(:from_dsm) { "dsm" }
  o
end

# def obj.m on a local written twice
def def_local(flag)
  o = Foo.new
  o = Foo.new if flag
  def o.from_def = "def"
  o
end

# class << obj on a local written twice: a def beside the others, one under an
# `if`, one an argument of `private`, two with a receiver (a class's own
# method and a `self.` one) and a `self.` one in a method of the body: nothing
# of the body runs, so Foo gets no method
def sclass_local(flag)
  o = Foo.new
  o = Foo.new if flag
  class << o
    def from_sclass = "sclass"
    if flag
      def from_sclass_if = "sclass if"
    end
    private def from_sclass_private = "sclass private"
    def Foo.from_sclass_class = "sclass class"
    def self.from_sclass_self = "sclass self"
    def from_sclass_setup
      def self.from_sclass_inner = "sclass inner"
    end
  end
  o
end

# define_singleton_method on an instance variable that is reset
class Holder
  def initialize = @o = Foo.new
  def reset = @o = Foo.new
  def go = @o.define_singleton_method(:from_ivar) { "ivar" }
end

# extend on a local written twice
def extend_local(flag)
  o = Foo.new
  o = Foo.new if flag
  o.extend(Tag)
  o
end

# a traced local beside them still defines its method on its object; a
# `def Foo.m` in a method of its `class << t` still defines Foo's own method
# when that method runs, and a `def self.m` there, one method deep or two,
# defines the object's own
t = Foo.new
t.define_singleton_method(:traced) { "traced" }
puts t.traced
class << t
  def define_marker
    def Foo.marker = "marker"
    def self.inner = "inner"
    :defined
  end
  def define_outer
    def define_middle
      def self.deep = "deep"
      :middle
    end
    :outer
  end
end
p t.define_marker
puts Foo.marker
p Foo.new.respond_to?(:marker)
puts t.inner
p t.define_outer
p t.define_middle
puts t.deep
p Foo.new.respond_to?(:inner)
p Foo.new.respond_to?(:deep)

%i[from_dsm from_def from_sclass from_sclass_if from_sclass_private from_sclass_setup
   from_sclass_inner from_ivar tagged].each do |name|
  puts "#{name}: #{respond_to?(name, true)}"
end
puts "Foo.from_sclass_class: #{Foo.respond_to?(:from_sclass_class)}"
puts "Foo.from_sclass_self: #{Foo.respond_to?(:from_sclass_self)}"
puts((begin; from_dsm; rescue NameError; "NameError"; end))
puts((begin; from_def; rescue NameError; "NameError"; end))
puts((begin; from_sclass; rescue NameError; "NameError"; end))
puts((begin; from_sclass_if; rescue NameError; "NameError"; end))
puts((begin; from_sclass_private; rescue NameError; "NameError"; end))
puts((begin; from_sclass_inner; rescue NameError; "NameError"; end))
puts((begin; from_ivar; rescue NameError; "NameError"; end))
puts((begin; traced; rescue NameError; "NameError"; end))
