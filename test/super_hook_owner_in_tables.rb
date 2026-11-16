# A `super` in a class method that a program's hook defines reaches the
# builtin's no-op only where the builtin has the method: Class has `inherited`
# and Module has `included`, but a module has no `inherited`, so a module's
# `def self.inherited(k) = super` raises NoMethodError.

class Base
  def self.inherited(k)
    puts "inherited #{k.name}"
    super
  end
  def self.method_added(m)
    super
  end
end
class Kid < Base
  def hi = 1
end

module Mod
  def self.included(k)
    puts "included in #{k.name}"
    super
  end
  def self.inherited(k) = super
end
class User
  include Mod
end

begin
  Mod.inherited(Class.new)
rescue NoMethodError => e
  puts e.class
end
