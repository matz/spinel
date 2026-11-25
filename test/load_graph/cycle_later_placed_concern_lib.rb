require_relative "cycle_later_placed_x_lib"
class Provider
  module Concern
    def debug = X.v
  end
end
