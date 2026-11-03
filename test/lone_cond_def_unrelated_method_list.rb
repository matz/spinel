# A lone def under a run-time condition in one class leaves another class's
# method lists answerable: instance_methods and its kin are decided by the
# receiver's chain, as they are beside an if/else pair of defs. (The class
# itself, a class below it, and a lone def in a module or Object still
# refuse such a list: whether the branch ran is not known at compile time.)

class Clock
  if ENV["SPINEL_TEST_NEVER_SET"]
    def monotonic = 1
  end

  def now = 2
end

class Wrapper
  def wrapped = :w
  def inspect = "#<Wrapper>"

  API = public_instance_methods(false).sort
end

class Plain; end

p Wrapper::API
p Wrapper.public_instance_methods(false).sort
p Wrapper.instance_methods(false).sort
p Plain.public_instance_methods(false)
p Wrapper.new.public_methods.include?(:wrapped)
p Wrapper.new.methods.include?(:monotonic)
p Clock.new.now
p Clock.new.respond_to?(:now)
