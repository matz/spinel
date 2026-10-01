# A class nested in a module under the name of a builtin exception that has
# no class id of its own (LoadError, SystemCallError) is that module's class,
# not the builtin (builtins/gem.rb's Gem::LoadError): a top-level reference
# still names the builtin, one inside the module names the nested class.
module Pkg
  class LoadError < ::LoadError; end
  class SystemCallError < StandardError; end
  def self.fail_inside = raise(LoadError, "inside")
  def self.sys = SystemCallError.new("s").class
end

begin
  raise LoadError, "plain"
rescue LoadError => e
  puts "ok #{e.message} #{e.class}"
end

begin
  raise Pkg::LoadError, "nested"
rescue LoadError => e
  puts "ok #{e.message} #{e.class}"
end

begin
  Pkg.fail_inside
rescue Pkg::LoadError => e
  puts "ok #{e.message} #{e.class}"
end
p Pkg.sys
