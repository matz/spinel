class ScopedSettings
  [:color, :size].each { |k| define_method("#{k}_label") { k.to_s } }
end

module ScopedLazy
  def autoload(*) = puts("user autoload")
end

module ScopedOverride
  extend ScopedLazy
end
ScopedOverride.autoload(:Ignored, File.join(__dir__, "autoload_scoped_lib"))

module ScopedMissing
  def method_missing(*) = puts("method_missing")
end

autoload :ScopedTop, File.join(__dir__, "autoload_scoped_lib")
class ScopedClass
  autoload :Entry, File.join(__dir__, "autoload_scoped_lib")
end
module ScopedModule
  extend ScopedMissing
  autoload :Entry, File.join(__dir__, "autoload_scoped_lib")
end

puts ScopedTop, ScopedClass::Entry, ScopedModule::Entry
puts ScopedSettings.new.color_label
