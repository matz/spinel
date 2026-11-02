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
# `if`, and one an argument of `private`
def sclass_local(flag)
  o = Foo.new
  o = Foo.new if flag
  class << o
    def from_sclass = "sclass"
    if flag
      def from_sclass_if = "sclass if"
    end
    private def from_sclass_private = "sclass private"
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

# a traced local beside them still defines its method on its object
t = Foo.new
t.define_singleton_method(:traced) { "traced" }
puts t.traced

%i[from_dsm from_def from_sclass from_sclass_if from_sclass_private from_ivar tagged].each do |name|
  puts "#{name}: #{respond_to?(name, true)}"
end
puts((begin; from_dsm; rescue NameError; "NameError"; end))
puts((begin; from_def; rescue NameError; "NameError"; end))
puts((begin; from_sclass; rescue NameError; "NameError"; end))
puts((begin; from_sclass_if; rescue NameError; "NameError"; end))
puts((begin; from_sclass_private; rescue NameError; "NameError"; end))
puts((begin; from_ivar; rescue NameError; "NameError"; end))
puts((begin; traced; rescue NameError; "NameError"; end))
