class Class
  def autoload(*) = puts("Class#autoload")
end

autoload :ClassReopenTop, File.join(__dir__, "autoload_class_reopen_lib")
module ClassReopenModule
  autoload :Entry, File.join(__dir__, "autoload_class_reopen_lib")
end

puts ClassReopenTop, ClassReopenModule::Entry
