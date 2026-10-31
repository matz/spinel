# The arguments of include, extend and prepend are constant reads that run
# where the call stands, so an autoloaded module's file loads there.
autoload :MixinInc, File.join(__dir__, "autoload_mixin_inc_lib")
autoload :MixinExt, File.join(__dir__, "autoload_mixin_ext_lib")
autoload :MixinPre, File.join(__dir__, "autoload_mixin_pre_lib")
autoload :MixinSend, File.join(__dir__, "autoload_mixin_send_lib")
puts "main"
class Holder
  include MixinInc
  extend MixinExt
  prepend MixinPre
end
puts "bodies run"
class Sent; end
Sent.include(MixinSend)
p Holder.new.inc, Holder.ext, Holder.new.pre, Sent.new.sent
