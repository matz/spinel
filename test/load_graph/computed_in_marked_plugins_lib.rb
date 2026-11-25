module Plugins
  def self.load(name)
    path = File.join(__dir__, "plugins", "#{name}.rb")
#<SPINEL_SOURCE>vendor/plugins/lib/plugins.rb:4
    require path if File.exist?(path)
    :loaded
  end
end
