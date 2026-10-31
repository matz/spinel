def autoload(*) = puts("user top")
self.autoload(:Anything, File.join(__dir__, "autoload_user_method"))
class UserAuto
  def autoload(*) = puts("user instance")
end
UserAuto.new.autoload(:Anything, File.join(__dir__, "autoload_lib"))
module UserAutoModule
  def self.autoload(*) = puts("user module")
end
UserAutoModule.autoload(:Anything, File.join(__dir__, "autoload_lib"))
