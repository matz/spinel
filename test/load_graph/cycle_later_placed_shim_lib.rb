require_relative "cycle_later_placed_esc_lib"
module Esc
  def self.escape(s) = "shim:#{s}"
end
