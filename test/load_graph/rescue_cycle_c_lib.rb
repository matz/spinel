begin
  require_relative "rescue_cycle_b_lib"
  def rescue_cycle_f = :new
rescue LoadError
  p :b_failed
end
