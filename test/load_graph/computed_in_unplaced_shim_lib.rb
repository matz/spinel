require_relative "computed_in_unplaced_esc_lib"
module Esc
  def self.escape(s) = "shim:#{s}"
end
