# Class#inherited fires where CRuby fires it: as each class is made, with
# the hook the superclass has at that point. A hook defined after a subclass
# does not fire for it, one removed before a subclass does not fire for it,
# one a module brings in with an extend fires for the subclasses after the
# extend, one under a run-time condition fires once its branch has run, and
# one an included hook installs with define_singleton_method fires for the
# classes made after the include.
class Late
end
class LateA < Late; end
class Late
  def self.inherited(k) = puts("Late #{k}")
end
class LateB < Late; end

class Gone
  def self.inherited(k) = puts("Gone #{k}")
end
class GoneA < Gone; end
class << Gone
  remove_method :inherited
end
class GoneB < Gone; end

module Brought
  def inherited(k) = puts("Brought #{k}")
end
class Ext; end
class ExtA < Ext; end
Ext.extend(Brought)
class ExtB < Ext; end

class Cond
  if ARGV.empty?
    def self.inherited(k) = puts("Cond #{k}")
  end
end
class CondA < Cond; end

module Tracking
  def self.included(base)
    base.define_singleton_method(:inherited) { |k| puts "tracked #{k}" }
  end
end
class Tracked; end
class TrackedA < Tracked; end
class Tracked; include Tracking; end
class TrackedB < Tracked; end
# (the module's own hook body leaves a function by that name: a class that
# has no hook does not call it)
Pair2 = Struct.new(:a)
class Pair2Sub < Pair2; end

module Plugin
  module ClassMethods
    def inherited(k)
      super
      puts "plugin #{k}"
    end
  end
  def self.included(base) = base.extend(ClassMethods)
end
class Host
  include Plugin
end
class HostA < Host; end

# a class made by Class.new(Base) fires before the constant is written
class Maker
  def self.inherited(k) = puts("Maker #{k.name.inspect}")
end
Made = Class.new(Maker)
puts Made.name

# a subclass of a class with no hook of its own, of a core class, of a Struct
# class with a hook in its block and of a name that stands for a class with a hook
class Quiet; end
class QuietA < Quiet; end
class Oops < StandardError; end
Pair = Struct.new(:a) do
  def self.inherited(k) = puts("Pair #{k}")
end
class PairSub < Pair; end
Alias = Late
class AliasSub < Alias; end
puts "done"
