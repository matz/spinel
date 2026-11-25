module Setup
  def self.run
    require_relative "computed_in_unplaced_plugins_lib"
    Plugins.load("none")
  end
end
