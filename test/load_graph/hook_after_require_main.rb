# A hook a required file defines fires for the classes and defs made after
# the require, not for those before it, and one in a file required under a
# run-time condition fires once the require has run; so does one for a
# remove_method in a `class << X` body.
class Base; end
class Gone
  def self.x; end
  def self.y; end
  class << self
    remove_method :x
  end
end
class Before < Base
  def early; end
end
require_relative "hook_after_require_lib"
class AfterRequire < Base
  def late_a; end
  def late_b; end
end

class << Gone
  remove_method :y
end

class Cond; end
require_relative "hook_after_require_cond_lib" if ARGV.empty?
class CondSub < Cond
  def cond_def; end
end
puts "done"
