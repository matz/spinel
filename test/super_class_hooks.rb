# `super` from a class-level hook reaches Module's or Class's own (a no-op
# answering nil), and an extended module's method is one entry in the
# ancestry: called on a subclass, its `super` goes past the module, not to
# the parent's copy of the same method. Thor's ClassMethods#method_added.
module Base
  module ClassMethods
    def desc(usage)
      @usage = usage
    end

    def method_added(meth)
      r = super
      if @usage.nil?
        puts "no usage for #{meth} in #{self} (super answered #{r.inspect})"
      else
        (@commands ||= {})[meth.to_s] = @usage
        @usage = nil
      end
    end


    def commands = @commands || {}
  end
end

class Thor
  extend Base::ClassMethods
end

class Project < Thor
  desc "add PATH"
  def add(path); end

  desc "ls"
  def ls; end

  def undocumented; end
end

p Project.commands
p Thor.commands

# the other core hooks answer nil to a `super` from the program's own
class Lifecycle
  def self.method_removed(n) = (puts "removed #{n}"; super)
  def self.method_undefined(n) = (puts "undefined #{n}"; super)
  def self.singleton_method_removed(n) = (puts "sremoved #{n}"; super)
  def self.singleton_method_undefined(n) = (puts "sundefined #{n}"; super)
  def self.inherited(k) = (puts "inherited #{k}"; super)
  p method_removed(:a), method_undefined(:b), singleton_method_removed(:c), singleton_method_undefined(:d)
end
