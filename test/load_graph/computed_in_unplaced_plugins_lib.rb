module Plugins
  def self.load(name)
    path = File.join(__dir__, "plugins", "#{name}.rb")
    require path if File.exist?(path)
    :loaded
  end
end
