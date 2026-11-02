# An inherited hook a required file defines fires for the classes made after
# the require, not for those before it, and one in a file required under a
# run-time condition fires once the require has run.
class Base; end
class Before < Base; end
require_relative "hook_after_require_lib"
class AfterRequire < Base; end

class Cond; end
require_relative "hook_after_require_cond_lib" if ARGV.empty?
class CondSub < Cond; end
puts "done"
