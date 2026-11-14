# A module's own respond_to_missing? (def self.) under a condition that did
# not run: the module answers false, and an instance of a class that
# includes it is no part of that.
module Util
  if ARGV.size > 0
    def self.respond_to_missing?(n, all = false) = n == :a
  end
end
class Host
  include Util
end
p Util.respond_to?(:a)
p Host.new.respond_to?(:a)
