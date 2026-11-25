require_relative "class_body_cycle_placed_rt_lib"

class Late
  require_relative "class_body_cycle_placed_msg_lib"
  def self.ready = :late
end
