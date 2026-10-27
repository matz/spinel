class Greeter
  include Polite
end
module Settings
  def setting = "on"
end
class << Config
  include Settings
end
module Helpers
  def helper = "helped"
end
extend Helpers
