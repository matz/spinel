require_relative "later_require_placed_esc_lib"
module Esc
  def self.escape(s) = "shim:#{s}"
end
